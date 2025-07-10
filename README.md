# Telegram 消息转发机器人

一个使用 C++ 编写的 Telegram 消息转发机器人，可以将用户消息转发给管理员，支持请求处理和双向通信。

## 功能特性

- 📨 **消息转发** - 自动将用户消息转发给管理员，包含用户名和 User ID
- 🔔 **请求系统** - 使用 `/req` 命令发送带操作按钮的请求（受理/拒绝/已完成）
- 💬 **双向通信** - 管理员可以通过回复转发的消息来回复用户
- ⚙️ **配置文件** - 支持从配置文件读取 Bot Token 和管理员 ID
- 📝 **日志记录** - 记录所有操作到日志文件

## 环境要求

- Debian 11 或其他 Linux 发行版
- CMake 3.10+
- C++14 编译器
- 网络连接（访问 Telegram API）

## 安装依赖

### Debian 11
```bash
# 更新系统
sudo apt update
sudo apt upgrade -y

# 安装编译工具
sudo apt install -y build-essential git cmake

# 安装必要的库
sudo apt install -y \
    libboost-all-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    zlib1g-dev
```

## 编译步骤

1. **克隆项目目录**

2. **创建构建目录并编译**
```bash
mkdir build
cd build
cmake ..
make -j$(nproc)
```

编译成功后，会在 `build` 目录下生成 `telegram_forward_bot` 可执行文件。

## 配置

1. **修改配置文件**

根据提示修改 `bot_config.ini` 文件

2. **获取必要信息**

**获取 Bot Token:**
- 在 Telegram 中搜索 @BotFather
- 发送 `/newbot` 创建新机器人
- 按提示设置机器人名称和用户名
- BotFather 会返回你的 Bot Token

**获取你的 Telegram ID:**
- 在 Telegram 中搜索 @userinfobot
- 发送任意消息
- 机器人会返回你的 User ID

## 运行

### 使用 Screen 运行（推荐）

```bash
# 创建新的 screen 会话
screen -S telegram_bot

# 运行机器人
cd /path/to/telegram_bot/build
./telegram_forward_bot

# 按 Ctrl+A 然后按 D 分离会话
# 机器人会在后台继续运行
```

### Screen 会话管理

```bash
# 查看所有 screen 会话
screen -ls

# 重新连接到机器人会话
screen -r telegram_bot

# 在会话中停止机器人
# 按 Ctrl+C

# 完全关闭会话
# 在会话中按 Ctrl+D 或输入 exit
```

### 直接运行（测试用）

```bash
cd build
./telegram_forward_bot
```

## 使用说明

### 用户端命令

- **发送普通消息** - 直接发送文本，机器人会转发给管理员
- `/start` - 显示欢迎信息和使用说明
- `/help` - 查看帮助信息  
- `/req <内容>` - 发送请求，管理员会看到操作按钮

示例：
```
/req 我需要申请一个新账号
```

### 管理员操作

1. **查看消息** - 所有用户消息都会转发到你的对话框，包含：
   - 用户名（@username 或姓名）
   - User ID
   - 时间戳
   - 消息内容

2. **回复消息** - 直接回复（Reply）转发过来的消息，内容会发送给对应用户

3. **处理请求** - 对于 `/req` 请求，点击相应按钮：
   - ✅ 受理 - 通知用户请求已受理
   - ❌ 拒绝 - 通知用户请求被拒绝
   - ✔️ 已完成 - 通知用户请求已完成

## 日志查看

```bash
# 实时查看日志
tail -f bot.log

# 查看最近的错误
grep ERROR bot.log
```

## 常见问题

### 1. 编译失败
- 确保安装了所有依赖库
- 检查 CMake 版本是否满足要求
- 查看编译错误信息，可能需要安装额外的开发包

### 2. 机器人无响应
- 检查 Bot Token 是否正确
- 确认网络连接正常
- 查看日志文件中的错误信息

### 3. 消息发送失败
- 确认管理员 ID 设置正确
- 用户可能屏蔽了机器人
- 检查 Telegram API 是否可访问

### 4. Screen 会话管理
```bash
# 如果意外断开连接，重新附加会话
screen -r telegram_bot

# 如果提示 "There is no screen to be resumed"
screen -ls  # 查看是否有其他会话名称

# 如果会话卡住，强制分离
screen -d telegram_bot
```

## 安全建议

1. **保护配置文件**
```bash
chmod 600 bot_config.ini
```

2. **定期备份配置**
```bash
cp bot_config.ini bot_config.ini.backup
```

3. **监控日志大小**
```bash
# 定期清理或轮转日志
ls -lh bot.log
```
