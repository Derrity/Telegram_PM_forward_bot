# Telegram 消息转发机器人

一个使用 C++ 编写的 Telegram 消息转发机器人，可以将用户消息转发给管理员，支持请求处理和双向通信。

## 功能特性

- 📨 **消息转发** - 自动将用户消息转发给管理员，包含用户名和 User ID
- 🔔 **请求系统** - 使用 `/req` 命令发送带操作按钮的请求（受理/拒绝/已完成）
- 💬 **双向通信** - 管理员可以通过回复转发的消息来回复用户
- 🚫 **用户封禁** - 支持封禁和解封用户，防止骚扰
- ⚡ **并发处理** - 多线程处理消息，避免阻塞
- ⚙️ **配置文件** - 灵活的配置选项
- 📝 **日志记录** - 详细的操作日志

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

### 👤 用户命令

| 命令          | 说明             | 示例               |
| ------------- | ---------------- | ------------------ |
| `/start`      | 开始使用机器人   | `/start`           |
| `/help`       | 查看帮助信息     | `/help`            |
| `/req <内容>` | 发送请求给管理员 | `/req 申请新账号`  |
| 直接发送消息  | 转发给管理员     | `你好，我需要帮助` |

### 👨‍💼 管理员命令

| 命令          | 说明         | 示例                      |
| ------------- | ------------ | ------------------------- |
| 回复消息      | 回复用户     | 直接回复转发的消息        |
| `/ban`        | 封禁用户     | 回复用户消息并发送 `/ban` |
| `/unban <ID>` | 解封用户     | `/unban 123456789`        |
| `/banlist`    | 查看封禁列表 | `/banlist`                |

### 管理员操作流程

#### 1. 处理普通消息
```
用户: 你好，我有个问题
↓ (自动转发)
管理员看到: 
💬 新消息
👤 用户: @username
🆔 ID: 123456789
📅 时间: 2025-07-11 14:24:26
━━━━━━━━━━━━━━━
💭 你好，我有个问题

管理员: [回复该消息] 你好，请问有什么可以帮助你的？
↓ (自动发送给用户)
用户收到: 💬 管理员回复:
你好，请问有什么可以帮助你的？
```

#### 2. 处理请求
```
用户: /req 申请开通高级功能
↓
管理员看到带按钮的消息:
📨 新请求
[✅ 受理] [❌ 拒绝] [✔️ 已完成]

管理员点击 [✅ 受理]
↓
用户收到: ✅ 您的请求已被受理！
```

#### 3. 封禁用户
```
收到骚扰消息 → 回复该消息输入 /ban → 用户被封禁
```

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
