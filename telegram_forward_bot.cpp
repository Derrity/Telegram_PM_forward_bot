#include <tgbot/tgbot.h>
#include <iostream>
#include <string>
#include <map>
#include <memory>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <signal.h>
#include <atomic>

// 全局运行标志
std::atomic<bool> running(true);

// 信号处理
void signalHandler(int signal) {
    std::cout << "\n收到信号 " << signal << "，正在关闭..." << std::endl;
    running = false;
}

// 配置类
class Config {
public:
    std::string botToken;
    int64_t adminId;
    int maxRetries = 3;
    int retryDelay = 5;
    bool enableLogging = true;
    std::string logFile = "bot.log";

    bool loadFromFile(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        while (std::getline(file, line)) {
            // 移除注释
            size_t commentPos = line.find('#');
            if (commentPos != std::string::npos) {
                line = line.substr(0, commentPos);
            }
            
            // 移除首尾空格
            line.erase(0, line.find_first_not_of(" \t"));
            line.erase(line.find_last_not_of(" \t") + 1);
            
            if (line.empty()) continue;

            // 解析键值对
            size_t pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                
                // 移除键值的空格
                key.erase(0, key.find_first_not_of(" \t"));
                key.erase(key.find_last_not_of(" \t") + 1);
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);

                if (key == "BOT_TOKEN") {
                    botToken = value;
                } else if (key == "ADMIN_ID") {
                    try {
                        adminId = std::stoll(value);
                    } catch (...) {
                        std::cerr << "无效的 ADMIN_ID: " << value << std::endl;
                        return false;
                    }
                } else if (key == "MAX_RETRIES") {
                    maxRetries = std::stoi(value);
                } else if (key == "RETRY_DELAY") {
                    retryDelay = std::stoi(value);
                } else if (key == "ENABLE_LOGGING") {
                    enableLogging = (value == "true" || value == "1");
                } else if (key == "LOG_FILE") {
                    logFile = value;
                }
            }
        }

        file.close();
        return !botToken.empty() && adminId != 0;
    }
};

// 日志类
class Logger {
private:
    std::ofstream logFile;
    std::mutex logMutex;
    bool enabled;

public:
    Logger(const std::string& filename, bool enable = true) : enabled(enable) {
        if (enabled) {
            logFile.open(filename, std::ios::app);
        }
    }

    ~Logger() {
        if (logFile.is_open()) {
            logFile.close();
        }
    }

    void log(const std::string& level, const std::string& message) {
        if (!enabled) return;

        std::lock_guard<std::mutex> lock(logMutex);
        
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
        ss << " [" << level << "] " << message;
        
        std::string logLine = ss.str();
        
        if (logFile.is_open()) {
            logFile << logLine << std::endl;
            logFile.flush();
        }
        
        std::cout << logLine << std::endl;
    }

    void info(const std::string& message) { log("INFO", message); }
    void warning(const std::string& message) { log("WARN", message); }
    void error(const std::string& message) { log("ERROR", message); }
};

// 消息缓存结构
struct CachedMessage {
    int64_t userId;
    std::string username;
    std::chrono::steady_clock::time_point timestamp;
};

// 主机器人类
class ForwardBot {
private:
    std::unique_ptr<TgBot::Bot> bot;
    int64_t adminId;
    Config config;
    std::unique_ptr<Logger> logger;
    
    // 线程安全的映射
    std::map<int64_t, CachedMessage> messageCache;
    std::mutex cacheMutex;
    
    // 回调查询超时管理
    std::map<std::string, std::chrono::steady_clock::time_point> callbackTimestamps;
    std::mutex callbackMutex;
    
    // 速率限制
    std::map<int64_t, std::chrono::steady_clock::time_point> lastMessageTime;
    std::mutex rateLimitMutex;

    // 清理过期缓存
    void cleanupCache() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(cacheMutex);
        
        auto it = messageCache.begin();
        while (it != messageCache.end()) {
            if (std::chrono::duration_cast<std::chrono::hours>(now - it->second.timestamp).count() > 24) {
                it = messageCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    // 检查速率限制
    bool checkRateLimit(int64_t userId) {
        std::lock_guard<std::mutex> lock(rateLimitMutex);
        auto now = std::chrono::steady_clock::now();
        
        auto it = lastMessageTime.find(userId);
        if (it != lastMessageTime.end()) {
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - it->second).count();
            if (diff < 1) { // 1秒内不能发送多条消息
                return false;
            }
        }
        
        lastMessageTime[userId] = now;
        return true;
    }

public:
    ForwardBot(const Config& cfg) 
        : config(cfg), adminId(cfg.adminId) {
        bot = std::make_unique<TgBot::Bot>(cfg.botToken);
        logger = std::make_unique<Logger>(cfg.logFile, cfg.enableLogging);
    }

    void start() {
        logger->info("机器人启动中...");
        logger->info("Admin ID: " + std::to_string(adminId));

        // 设置命令
        setupCommands();
        
        // 设置消息处理器
        setupMessageHandlers();
        
        // 设置回调查询处理器
        setupCallbackHandlers();

        // 启动清理线程
        std::thread cleanupThread([this]() {
            while (running) {
                std::this_thread::sleep_for(std::chrono::hours(1));
                cleanupCache();
            }
        });

        // 主循环
        try {
            logger->info("机器人已启动，等待消息...");
            TgBot::TgLongPoll longPoll(*bot);
            
            while (running) {
                try {
                    longPoll.start();
                } catch (std::exception& e) {
                    logger->error("轮询错误: " + std::string(e.what()));
                    if (running) {
                        std::this_thread::sleep_for(std::chrono::seconds(config.retryDelay));
                    }
                }
            }
        } catch (std::exception& e) {
            logger->error("致命错误: " + std::string(e.what()));
        }

        cleanupThread.join();
        logger->info("机器人已停止");
    }

private:
    void setupCommands() {
        // /start 命令
        bot->getEvents().onCommand("start", [this](TgBot::Message::Ptr message) {
            try {
                std::string welcomeMsg = 
                    "🤖 *欢迎使用消息转发机器人*\n\n"
                    "📝 *使用说明:*\n"
                    "• 直接发送消息 - 转发给管理员\n"
                    "• /req \\<内容\\> - 发送请求\n"
                    "• /help - 查看帮助\n"
                    "• /status - 查看状态\n\n"
                    "💡 管理员会尽快回复您的消息";
                
                bot->getApi().sendMessage(
                    message->chat->id, 
                    welcomeMsg,
                    nullptr, nullptr, nullptr, 
                    "MarkdownV2"
                );
                
                logger->info("用户 " + std::to_string(message->from->id) + " 启动了机器人");
            } catch (std::exception& e) {
                logger->error("处理 /start 命令失败: " + std::string(e.what()));
            }
        });

        // /help 命令
        bot->getEvents().onCommand("help", [this](TgBot::Message::Ptr message) {
            std::string helpMsg = 
                "📋 *帮助信息*\n\n"
                "*可用命令:*\n"
                "/start - 开始使用\n"
                "/help - 显示此帮助\n"
                "/req - 发送请求\n"
                "/status - 查看机器人状态\n\n"
                "*使用示例:*\n"
                "`/req 我需要帮助解决一个问题`";
            
            sendMessage(message->chat->id, helpMsg, true);
        });

        // /status 命令
        bot->getEvents().onCommand("status", [this](TgBot::Message::Ptr message) {
            std::string status = "✅ 机器人运行正常\n";
            status += "📊 缓存消息数: " + std::to_string(messageCache.size());
            
            sendMessage(message->chat->id, status);
        });

        // /req 命令
        bot->getEvents().onCommand("req", [this](TgBot::Message::Ptr message) {
            handleRequestCommand(message);
        });
    }

    void setupMessageHandlers() {
        bot->getEvents().onAnyMessage([this](TgBot::Message::Ptr message) {
            try {
                // 跳过命令消息
                if (!message->text.empty() && message->text[0] == '/') {
                    return;
                }

                // 检查速率限制
                if (!checkRateLimit(message->from->id)) {
                    sendMessage(message->chat->id, "⚠️ 请慢一点发送消息");
                    return;
                }

                if (message->chat->id == adminId) {
                    handleAdminReply(message);
                } else {
                    forwardToAdmin(message);
                }
            } catch (std::exception& e) {
                logger->error("处理消息失败: " + std::string(e.what()));
            }
        });
    }

    void setupCallbackHandlers() {
        bot->getEvents().onCallbackQuery([this](TgBot::CallbackQuery::Ptr query) {
            try {
                // 检查回调是否过期
                {
                    std::lock_guard<std::mutex> lock(callbackMutex);
                    auto now = std::chrono::steady_clock::now();
                    
                    // 清理过期的回调记录
                    auto it = callbackTimestamps.begin();
                    while (it != callbackTimestamps.end()) {
                        if (std::chrono::duration_cast<std::chrono::minutes>(now - it->second).count() > 60) {
                            it = callbackTimestamps.erase(it);
                        } else {
                            ++it;
                        }
                    }
                    
                    // 检查当前回调
                    if (callbackTimestamps.find(query->id) != callbackTimestamps.end()) {
                        bot->getApi().answerCallbackQuery(query->id, "此操作已处理过");
                        return;
                    }
                    
                    callbackTimestamps[query->id] = now;
                }
                
                handleCallbackQuery(query);
            } catch (std::exception& e) {
                logger->error("处理回调查询失败: " + std::string(e.what()));
                try {
                    bot->getApi().answerCallbackQuery(query->id, "操作失败，请重试");
                } catch (...) {}
            }
        });
    }

    void handleRequestCommand(TgBot::Message::Ptr message) {
        std::string requestText = message->text;
        if (requestText.length() > 5) {
            requestText = requestText.substr(5);
        } else {
            sendMessage(message->chat->id, "❌ 请在 /req 后面输入你的请求内容");
            return;
        }

        // 创建内联键盘
        auto keyboard = createRequestKeyboard(message->messageId);

        // 构建消息
        std::stringstream ss;
        ss << "📨 *新请求*\n\n";
        ss << "👤 用户: " << escapeMarkdownV2(getUserDisplay(message->from)) << "\n";
        ss << "🆔 ID: `" << message->from->id << "`\n";
        ss << "📅 时间: " << getCurrentTime() << "\n";
        ss << "━━━━━━━━━━━━━━━\n";
        ss << "📝 " << escapeMarkdownV2(requestText);

        auto sentMessage = sendMessage(adminId, ss.str(), true, keyboard);
        
        if (sentMessage) {
            // 缓存消息信息
            std::lock_guard<std::mutex> lock(cacheMutex);
            messageCache[sentMessage->messageId] = {
                message->from->id,
                getUserDisplay(message->from),
                std::chrono::steady_clock::now()
            };

            sendMessage(message->chat->id, "✅ 您的请求已发送给管理员，请耐心等待处理。");
            logger->info("收到来自用户 " + std::to_string(message->from->id) + " 的请求");
        }
    }

    void forwardToAdmin(TgBot::Message::Ptr message) {
        std::stringstream ss;
        ss << "💬 *新消息*\n\n";
        ss << "👤 用户: " << escapeMarkdownV2(getUserDisplay(message->from)) << "\n";
        ss << "🆔 ID: `" << message->from->id << "`\n";
        ss << "📅 时间: " << getCurrentTime() << "\n";
        ss << "━━━━━━━━━━━━━━━\n";
        ss << "💭 " << escapeMarkdownV2(message->text);

        auto sentMessage = sendMessage(adminId, ss.str(), true);
        
        if (sentMessage) {
            std::lock_guard<std::mutex> lock(cacheMutex);
            messageCache[sentMessage->messageId] = {
                message->from->id,
                getUserDisplay(message->from),
                std::chrono::steady_clock::now()
            };

            logger->info("转发消息从用户 " + std::to_string(message->from->id));
        }
    }

    void handleAdminReply(TgBot::Message::Ptr message) {
        if (!message->replyToMessage) {
            return;
        }

        int64_t userId = 0;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = messageCache.find(message->replyToMessage->messageId);
            if (it == messageCache.end()) {
                sendMessage(adminId, "⚠️ 找不到对应的用户信息（可能已过期）");
                return;
            }
            userId = it->second.userId;
        }

        std::string replyText = "💬 *管理员回复:*\n\n" + escapeMarkdownV2(message->text);
        
        if (sendMessage(userId, replyText, true)) {
            sendMessage(adminId, "✅ 消息已发送");
            logger->info("管理员回复用户 " + std::to_string(userId));
        } else {
            sendMessage(adminId, "❌ 发送失败，用户可能已屏蔽机器人");
        }
    }

    void handleCallbackQuery(TgBot::CallbackQuery::Ptr query) {
        std::string data = query->data;
        std::string action;
        
        size_t pos = data.find('_');
        if (pos != std::string::npos) {
            action = data.substr(0, pos);
        }

        int64_t userId = 0;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = messageCache.find(query->message->messageId);
            if (it == messageCache.end()) {
                bot->getApi().answerCallbackQuery(query->id, "❌ 请求信息已过期");
                return;
            }
            userId = it->second.userId;
        }

        std::string response, adminNotification;
        
        if (action == "accept") {
            response = "✅ *您的请求已被受理*\n\n管理员正在处理您的请求，请耐心等待。";
            adminNotification = "✅ 已受理";
        } else if (action == "reject") {
            response = "❌ *您的请求已被拒绝*\n\n如有疑问，请重æ交更详细的信息。";
            adminNotification = "❌ 已拒绝";
        } else if (action == "complete") {
            response = "✔️ *您的请求已完成*\n\n感谢您的耐心等待！";
            adminNotification = "✔️ 已完成";
        }

        if (sendMessage(userId, response, true)) {
            // 更新管理员消息
            try {
                std::string updatedText = query->message->text + "\n\n📌 状态: " + adminNotification;
                bot->getApi().editMessageText(
                    updatedText,
                    adminId,
                    query->message->messageId
                );
                
                bot->getApi().answerCallbackQuery(query->id, "✅ 操作成功");
                logger->info("处理请求状态更新: " + adminNotification);
            } catch (...) {
                bot->getApi().answerCallbackQuery(query->id, "操作成功但无法更新消息");
            }
        } else {
            bot->getApi().answerCallbackQuery(query->id, "❌ 通知用户失败");
        }
    }

    // 辅助函数
    TgBot::InlineKeyboardMarkup::Ptr createRequestKeyboard(int64_t messageId) {
        auto keyboard = std::make_shared<TgBot::InlineKeyboardMarkup>();
        std::vector<TgBot::InlineKeyboardButton::Ptr> row;
        
        auto acceptBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        acceptBtn->text = "✅ 受理";
        acceptBtn->callbackData = "accept_" + std::to_string(messageId);
        row.push_back(acceptBtn);

        auto rejectBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        rejectBtn->text = "❌ 拒绝";
        rejectBtn->callbackData = "reject_" + std::to_string(messageId);
        row.push_back(rejectBtn);

        auto completeBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        completeBtn->text = "✔️ 已完成";
        completeBtn->callbackData = "complete_" + std::to_string(messageId);
        row.push_back(completeBtn);

        keyboard->inlineKeyboard.push_back(row);
        return keyboard;
    }

    TgBot::Message::Ptr sendMessage(int64_t chatId, const std::string& text, 
                                   bool useMarkdown = false, 
                                   TgBot::InlineKeyboardMarkup::Ptr keyboard = nullptr) {
        int retries = 0;
        while (retries < config.maxRetries) {
            try {
                return bot->getApi().sendMessage(
                    chatId, text, nullptr, nullptr, keyboard,
                    useMarkdown ? "MarkdownV2" : ""
                );
            } catch (std::exception& e) {
                logger->error("发送消息失败 (尝试 " + std::to_string(retries + 1) + 
                            "/" + std::to_string(config.maxRetries) + "): " + std::string(e.what()));
                retries++;
                if (retries < config.maxRetries) {
                    std::this_thread::sleep_for(std::chrono::seconds(config.retryDelay));
                }
            }
        }
        return nullptr;
    }

    std::string escapeMarkdownV2(const std::string& text) {
        std::string result;
        for (char c : text) {
            if (c == '_' || c == '*' || c == '[' || c == ']' || c == '(' || 
                c == ')' || c == '~' || c == '`' || c == '>' || c == '#' || 
                c == '+' || c == '-' || c == '=' || c == '|' || c == '{' || 
                c == '}' || c == '.' || c == '!' || c == '\\') {
                result += '\\';
            }
            result += c;
        }
        return result;
    }

    std::string getUserDisplay(TgBot::User::Ptr user) {
        if (!user->username.empty()) {
            return "@" + user->username;
        }
        
        std::string name = user->firstName;
        if (!user->lastName.empty()) {
            name += " " + user->lastName;
        }
        return name;
    }

    std::string getCurrentTime() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::stringstream ss;
        ss << std::put_time(std::localtime(&time_t), "%Y\\-%m\\-%d %H:%M:%S");
        return ss.str();
    }
};

int main(int argc, char* argv[]) {
    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 配置文件路径
    std::string configFile = "bot_config.ini";
    if (argc > 1) {
        configFile = argv[1];
    }

    // 加载配置
    Config config;
    if (!config.loadFromFile(configFile)) {
        std::cerr << "错误: 无法加载配置文件 " << configFile << std::endl;
        std::cerr << "\n请创建配置文件，格式如下：" << std::endl;
        std::cerr << "BOT_TOKEN=your_bot_token_here" << std::endl;
        std::cerr << "ADMIN_ID=your_telegram_id" << std::endl;
        std::cerr << "MAX_RETRIES=3" << std::endl;
        std::cerr << "RETRY_DELAY=5" << std::endl;
        std::cerr << "ENABLE_LOGGING=true" << std::endl;
        std::cerr << "LOG_FILE=bot.log" << std::endl;
        return 1;
    }

    try {
        ForwardBot bot(config);
        bot.start();
    } catch (std::exception& e) {
        std::cerr << "致命错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
