#include <tgbot/tgbot.h>
#include <iostream>
#include <string>
#include <map>
#include <set>
#include <memory>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <queue>
#include <condition_variable>
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
    bool enableLogging = true;
    std::string logFile = "bot.log";
    std::string bannedUsersFile = "banned_users.txt";
    int workerThreads = 4; // 工作线程数

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
                } else if (key == "ENABLE_LOGGING") {
                    enableLogging = (value == "true" || value == "1");
                } else if (key == "LOG_FILE") {
                    logFile = value;
                } else if (key == "BANNED_USERS_FILE") {
                    bannedUsersFile = value;
                } else if (key == "WORKER_THREADS") {
                    try {
                        workerThreads = std::stoi(value);
                    } catch (...) {
                        workerThreads = 4;
                    }
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
    void error(const std::string& message) { log("ERROR", message); }
    void warning(const std::string& message) { log("WARN", message); }
};

// 消息任务
struct MessageTask {
    enum Type { FORWARD_TO_ADMIN, REPLY_TO_USER, HANDLE_CALLBACK, HANDLE_REQUEST };
    Type type;
    TgBot::Message::Ptr message;
    TgBot::CallbackQuery::Ptr callbackQuery;
    int64_t targetUserId;
    std::string text;
};

// 主机器人类
class ForwardBot {
private:
    std::unique_ptr<TgBot::Bot> bot;
    int64_t adminId;
    Config config;
    std::unique_ptr<Logger> logger;
    
    // 消息映射
    std::map<int64_t, std::pair<int64_t, std::string>> messageCache; // messageId -> (userId, username)
    std::mutex cacheMutex;
    
    // 封禁用户列表
    std::set<int64_t> bannedUsers;
    std::mutex bannedMutex;
    
    // 回调查询记录
    std::map<std::string, std::chrono::steady_clock::time_point> processedCallbacks;
    std::mutex callbackMutex;
    
    // 消息队列和工作线程
    std::queue<MessageTask> taskQueue;
    std::mutex queueMutex;
    std::condition_variable queueCV;
    std::vector<std::thread> workers;
    std::atomic<bool> stopWorkers{false};

    // 加载封禁用户列表
    void loadBannedUsers() {
        std::ifstream file(config.bannedUsersFile);
        if (file.is_open()) {
            std::lock_guard<std::mutex> lock(bannedMutex);
            int64_t userId;
            while (file >> userId) {
                bannedUsers.insert(userId);
            }
            file.close();
            logger->info("加载了 " + std::to_string(bannedUsers.size()) + " 个封禁用户");
        }
    }

    // 保存封禁用户列表
    void saveBannedUsers() {
        std::ofstream file(config.bannedUsersFile);
        if (file.is_open()) {
            std::lock_guard<std::mutex> lock(bannedMutex);
            for (const auto& userId : bannedUsers) {
                file << userId << std::endl;
            }
            file.close();
        }
    }

    // 检查用户是否被封禁
    bool isUserBanned(int64_t userId) {
        std::lock_guard<std::mutex> lock(bannedMutex);
        return bannedUsers.find(userId) != bannedUsers.end();
    }

    // 封禁用户
    void banUser(int64_t userId) {
        {
            std::lock_guard<std::mutex> lock(bannedMutex);
            bannedUsers.insert(userId);
        }
        saveBannedUsers();
    }

    // 解封用户
    void unbanUser(int64_t userId) {
        {
            std::lock_guard<std::mutex> lock(bannedMutex);
            bannedUsers.erase(userId);
        }
        saveBannedUsers();
    }

    // 工作线程函数
    void workerThread() {
        while (!stopWorkers) {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCV.wait(lock, [this] { return !taskQueue.empty() || stopWorkers; });
            
            if (stopWorkers) break;
            
            if (!taskQueue.empty()) {
                MessageTask task = taskQueue.front();
                taskQueue.pop();
                lock.unlock();
                
                // 处理任务
                processTask(task);
            }
        }
    }

    // 处理任务
    void processTask(const MessageTask& task) {
        try {
            switch (task.type) {
                case MessageTask::FORWARD_TO_ADMIN:
                    processForwardToAdmin(task.message);
                    break;
                case MessageTask::REPLY_TO_USER:
                    processReplyToUser(task.targetUserId, task.text);
                    break;
                case MessageTask::HANDLE_CALLBACK:
                    processCallbackQuery(task.callbackQuery);
                    break;
                case MessageTask::HANDLE_REQUEST:
                    processRequestCommand(task.message);
                    break;
            }
        } catch (std::exception& e) {
            logger->error("处理任务失败: " + std::string(e.what()));
        }
    }

    // 添加任务到队列
    void addTask(const MessageTask& task) {
        std::lock_guard<std::mutex> lock(queueMutex);
        taskQueue.push(task);
        queueCV.notify_one();
    }

public:
    ForwardBot(const Config& cfg)
        : config(cfg), adminId(cfg.adminId), stopWorkers(false) {
        bot = std::make_unique<TgBot::Bot>(cfg.botToken);
        logger = std::make_unique<Logger>(cfg.logFile, cfg.enableLogging);
        
        // 加载封禁用户
        loadBannedUsers();
        
        // 启动工作线程
        for (int i = 0; i < cfg.workerThreads; ++i) {
            workers.emplace_back(&ForwardBot::workerThread, this);
        }
    }

    ~ForwardBot() {
        // 停止工作线程
        stopWorkers = true;
        queueCV.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void start() {
        logger->info("机器人启动中...");
        logger->info("Admin ID: " + std::to_string(adminId));
        logger->info("工作线程数: " + std::to_string(config.workerThreads));

        // /start 命令
        bot->getEvents().onCommand("start", [this](TgBot::Message::Ptr message) {
            if (isUserBanned(message->from->id)) {
                return; // 忽略被封禁用户
            }
            
            bot->getApi().sendMessage(message->chat->id,
                "🤖 欢迎使用消息转发机器人！\n\n"
                "📝 使用说明:\n"
                "• 直接发送消息 - 转发给管理员\n"
                "• /req <内容> - 发送带按钮的请求\n"
                "• /help - 查看帮助\n\n"
                "管理员会尽快回复您的消息！");
            
            logger->info("用户 " + std::to_string(message->from->id) + " 启动了机器人");
        });

        // /help 命令
        bot->getEvents().onCommand("help", [this](TgBot::Message::Ptr message) {
            if (isUserBanned(message->from->id)) {
                return;
            }
            
            bot->getApi().sendMessage(message->chat->id,
                "📋 帮助信息\n\n"
                "可用命令:\n"
                "/start - 开始使用\n"
                "/help - 显示帮助\n"
                "/req - 发送请求\n\n"
                "使用示例:\n"
                "/req 我需要帮助解决一个问题");
        });

        // /req 命令
        bot->getEvents().onCommand("req", [this](TgBot::Message::Ptr message) {
            if (isUserBanned(message->from->id)) {
                bot->getApi().sendMessage(message->chat->id, "❌ 您已被限制使用此功能");
                return;
            }
            
            MessageTask task;
            task.type = MessageTask::HANDLE_REQUEST;
            task.message = message;
            addTask(task);
        });

        // 管理员命令
        bot->getEvents().onCommand("ban", [this](TgBot::Message::Ptr message) {
            if (message->chat->id != adminId) return;
            handleBanCommand(message);
        });

        bot->getEvents().onCommand("unban", [this](TgBot::Message::Ptr message) {
            if (message->chat->id != adminId) return;
            handleUnbanCommand(message);
        });

        bot->getEvents().onCommand("banlist", [this](TgBot::Message::Ptr message) {
            if (message->chat->id != adminId) return;
            showBannedList();
        });

        // 处理普通消息
        bot->getEvents().onAnyMessage([this](TgBot::Message::Ptr message) {
            try {
                // 跳过命令消息
                if (!message->text.empty() && message->text[0] == '/') {
                    return;
                }

                if (message->chat->id == adminId) {
                    handleAdminReply(message);
                } else {
                    if (isUserBanned(message->from->id)) {
                        // 可选：通知用户已被封禁
                        // bot->getApi().sendMessage(message->chat->id, "❌ 您已被限制发送消息");
                        logger->info("已拦截被封禁用户 " + std::to_string(message->from->id) + " 的消息");
                        return;
                    }
                    
                    MessageTask task;
                    task.type = MessageTask::FORWARD_TO_ADMIN;
                    task.message = message;
                    addTask(task);
                }
            } catch (std::exception& e) {
                logger->error("处理消息失败: " + std::string(e.what()));
            }
        });

        // 处理回调查询
        bot->getEvents().onCallbackQuery([this](TgBot::CallbackQuery::Ptr query) {
            MessageTask task;
            task.type = MessageTask::HANDLE_CALLBACK;
            task.callbackQuery = query;
            addTask(task);
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
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                    }
                }
            }
        } catch (std::exception& e) {
            logger->error("致命错误: " + std::string(e.what()));
        }

        logger->info("机器人已停止");
    }

private:
    void handleBanCommand(TgBot::Message::Ptr message) {
        if (!message->replyToMessage) {
            bot->getApi().sendMessage(adminId, "❌ 请回复要封禁的用户消息并使用 /ban");
            return;
        }

        int64_t userId = 0;
        std::string username;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = messageCache.find(message->replyToMessage->messageId);
            if (it == messageCache.end()) {
                bot->getApi().sendMessage(adminId, "⚠️ 找不到对应的用户信息");
                return;
            }
            userId = it->second.first;
            username = it->second.second;
        }

        banUser(userId);
        bot->getApi().sendMessage(adminId,
            "🚫 已封禁用户 " + username + " (ID: " + std::to_string(userId) + ")");
        logger->info("封禁用户: " + std::to_string(userId));
    }

    void handleUnbanCommand(TgBot::Message::Ptr message) {
        std::string text = message->text;
        if (text.length() <= 7) { // "/unban "
            bot->getApi().sendMessage(adminId, "❌ 用法: /unban <user_id>");
            return;
        }

        try {
            int64_t userId = std::stoll(text.substr(7));
            unbanUser(userId);
            bot->getApi().sendMessage(adminId,
                "✅ 已解封用户 ID: " + std::to_string(userId));
            logger->info("解封用户: " + std::to_string(userId));
        } catch (...) {
            bot->getApi().sendMessage(adminId, "❌ 无效的用户 ID");
        }
    }

    void showBannedList() {
        std::lock_guard<std::mutex> lock(bannedMutex);
        if (bannedUsers.empty()) {
            bot->getApi().sendMessage(adminId, "📋 封禁列表为空");
            return;
        }

        std::stringstream ss;
        ss << "🚫 封禁用户列表:\n\n";
        for (const auto& userId : bannedUsers) {
            ss << "• " << userId << "\n";
        }
        ss << "\n使用 /unban <user_id> 解封用户";

        bot->getApi().sendMessage(adminId, ss.str());
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
                bot->getApi().sendMessage(adminId, "⚠️ 找不到对应的用户信息");
                return;
            }
            userId = it->second.first;
        }

        MessageTask task;
        task.type = MessageTask::REPLY_TO_USER;
        task.targetUserId = userId;
        task.text = message->text;
        addTask(task);
    }

    void processRequestCommand(TgBot::Message::Ptr message) {
        std::string requestText = message->text;
        if (requestText.length() > 5) {
            requestText = requestText.substr(5);
        } else {
            bot->getApi().sendMessage(message->chat->id, "❌ 请在 /req 后面输入你的请求内容");
            return;
        }

        // 创建内联键盘
        auto keyboard = std::make_shared<TgBot::InlineKeyboardMarkup>();
        std::vector<TgBot::InlineKeyboardButton::Ptr> row;
        
        auto acceptBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        acceptBtn->text = "✅ 受理";
        acceptBtn->callbackData = "accept_" + std::to_string(message->messageId);
        row.push_back(acceptBtn);

        auto rejectBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        rejectBtn->text = "❌ 拒绝";
        rejectBtn->callbackData = "reject_" + std::to_string(message->messageId);
        row.push_back(rejectBtn);

        auto completeBtn = std::make_shared<TgBot::InlineKeyboardButton>();
        completeBtn->text = "✔️ 已完成";
        completeBtn->callbackData = "complete_" + std::to_string(message->messageId);
        row.push_back(completeBtn);

        keyboard->inlineKeyboard.push_back(row);

        // 构建消息
        std::stringstream ss;
        ss << "📨 新请求\n\n";
        ss << "👤 用户: " << getUserDisplay(message->from) << "\n";
        ss << "🆔 ID: " << message->from->id << "\n";
        ss << "📅 时间: " << getCurrentTime() << "\n";
        ss << "━━━━━━━━━━━━━━━\n";
        ss << "📝 " << requestText;

        try {
            auto sentMessage = bot->getApi().sendMessage(adminId, ss.str(), nullptr, nullptr, keyboard);
            
            // 缓存消息信息
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                messageCache[sentMessage->messageId] = {message->from->id, getUserDisplay(message->from)};
            }

            bot->getApi().sendMessage(message->chat->id, "✅ 您的请求已发送给管理员，请耐心等待处理。");
            logger->info("收到请求 - 用户: " + std::to_string(message->from->id));
        } catch (std::exception& e) {
            bot->getApi().sendMessage(message->chat->id, "❌ 发送失败，请稍后重试");
            logger->error("发送请求失败: " + std::string(e.what()));
        }
    }

    void processForwardToAdmin(TgBot::Message::Ptr message) {
        std::stringstream ss;
        ss << "💬 新消息\n\n";
        ss << "👤 用户: " << getUserDisplay(message->from) << "\n";
        ss << "🆔 ID: " << message->from->id << "\n";
        ss << "📅 时间: " << getCurrentTime() << "\n";
        ss << "━━━━━━━━━━━━━━━\n";
        ss << "💭 " << message->text;

        try {
            auto sentMessage = bot->getApi().sendMessage(adminId, ss.str());
            
            {
                std::lock_guard<std::mutex> lock(cacheMutex);
                messageCache[sentMessage->messageId] = {message->from->id, getUserDisplay(message->from)};
            }

            logger->info("转发消息 - 用户: " + std::to_string(message->from->id));
        } catch (std::exception& e) {
            logger->error("转发消息失败: " + std::string(e.what()));
        }
    }

    void processReplyToUser(int64_t userId, const std::string& text) {
        try {
            bot->getApi().sendMessage(userId, "💬 管理员回复:\n\n" + text);
            bot->getApi().sendMessage(adminId, "✅ 消息已发送");
            logger->info("管理员回复用户 " + std::to_string(userId));
        } catch (std::exception& e) {
            bot->getApi().sendMessage(adminId, "❌ 发送失败: " + std::string(e.what()));
            logger->error("回复失败: " + std::string(e.what()));
        }
    }

    void processCallbackQuery(TgBot::CallbackQuery::Ptr query) {
        // 检查是否已处理过
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            auto now = std::chrono::steady_clock::now();
            
            // 清理旧记录
            auto it = processedCallbacks.begin();
            while (it != processedCallbacks.end()) {
                if (std::chrono::duration_cast<std::chrono::hours>(now - it->second).count() > 1) {
                    it = processedCallbacks.erase(it);
                } else {
                    ++it;
                }
            }
            
            // 检查当前回调
            if (processedCallbacks.find(query->id) != processedCallbacks.end()) {
                bot->getApi().answerCallbackQuery(query->id, "此操作已处理");
                return;
            }
            processedCallbacks[query->id] = now;
        }

        std::string data = query->data;
        std::string action = data.substr(0, data.find('_'));

        int64_t userId = 0;
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            auto it = messageCache.find(query->message->messageId);
            if (it == messageCache.end()) {
                bot->getApi().answerCallbackQuery(query->id, "❌ 请求信息不存在");
                return;
            }
            userId = it->second.first;
        }

        std::string response, status;
        
        if (action == "accept") {
            response = "✅ 您的请求已被受理！\n管理员正在处理中...";
            status = "✅ 已受理";
        } else if (action == "reject") {
            response = "❌ 您的请求已被拒绝。\n如有需要请重新提交。";
            status = "❌ 已拒绝";
        } else if (action == "complete") {
            response = "✔️ 您的请求已完成！\n感谢您的耐心等待。";
            status = "✔️ 已完成";
        }

        try {
            bot->getApi().sendMessage(userId, response);
            
            // 更新消息
            std::string updatedText = query->message->text + "\n\n📌 状态: " + status;
            bot->getApi().editMessageText(updatedText, adminId, query->message->messageId);
            
            bot->getApi().answerCallbackQuery(query->id, "✅ 操作成功");
            logger->info("处理请求 - 状态: " + status + " 用户: " + std::to_string(userId));
        } catch (std::exception& e) {
            bot->getApi().answerCallbackQuery(query->id, "操作失败");
            logger->error("处理回调失败: " + std::string(e.what()));
        }
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
        ss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
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
        std::cerr << "WORKER_THREADS=4" << std::endl;
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
