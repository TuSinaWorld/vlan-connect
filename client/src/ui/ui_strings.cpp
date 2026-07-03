#include "ui_strings.h"

#include <cstddef>

namespace VLan {
namespace {

AppLanguage g_language = AppLanguage::English;

struct UiEntry {
    const char* key;
    const char* en;
    const char* zh;
};

const UiEntry kEntries[] = {
    {"app.windowTitle", "VLan - Virtual LAN Client", "VLan - 虚拟局域网客户端"},

    {"nav.login", "Login", "登录"},
    {"nav.lobby", "Lobby", "房间大厅"},
    {"nav.create", "Create", "创建"},
    {"nav.members", "Members", "成员"},
    {"nav.logs", "Logs", "日志"},
    {"nav.settings", "Settings", "设置"},

    {"page.login.title", "Login", "登录"},
    {"page.login.subtitle", "Connect to a server and sign in to VLan", "连接服务器并登录到 VLan"},
    {"page.lobby.title", "Lobby", "房间大厅"},
    {"page.lobby.subtitle", "Browse available rooms, join, or leave the current room", "浏览可用房间，加入或离开当前房间"},
    {"page.create.title", "Create room", "创建房间"},
    {"page.create.subtitle", "Configure players, room password, transport, FEC, and MTU", "配置人数、房间密码、传输、FEC 与 MTU"},
    {"page.members.title", "Members", "成员"},
    {"page.members.subtitle", "View peers, virtual IPs, transports, and latency", "查看成员、虚拟 IP、传输方式与延迟"},
    {"page.logs.title", "Logs", "日志"},
    {"page.logs.subtitle", "Observe connection, room, and tunnel events", "查看连接、房间和隧道事件"},
    {"page.settings.title", "Settings", "设置"},
    {"page.settings.subtitle", "Language and local defaults", "语言和本机默认配置"},

    {"brand.subtitle", "Virtual LAN Console", "虚拟局域网控制台"},
    {"shell.badge", "VLan Client", "VLan 客户端"},

    {"metric.server", "Server", "服务器"},
    {"metric.room", "Room", "房间"},
    {"metric.peer", "Peer", "身份"},
    {"metric.serverRtt", "Server RTT", "服务器延迟"},
    {"metric.disconnected", "Disconnected", "未连接"},
    {"metric.connected", "Connected", "已连接"},
    {"metric.connecting", "Connecting", "连接中"},
    {"metric.notJoined", "Not joined", "未加入房间"},
    {"metric.roomValue", "Room %1", "房间 %1"},
    {"metric.peerValue", "Peer %1", "Peer %1"},
    {"metric.loggedIn", "Logged in (ID=%1)", "已登录 (ID=%1)"},
    {"metric.connectFailed", "Connect failed", "连接失败"},

    {"traffic.title", "Tunnel speed", "隧道速度"},
    {"traffic.value", "Up %1  Down %2", "上行 %1  下行 %2"},

    {"login.card", "Server Connection", "服务器连接"},
    {"login.server", "Server", "服务器"},
    {"login.serverPlaceholder", "Server address, optional port, default 11510", "服务器地址，可带端口，默认 11510"},
    {"login.name", "Name", "昵称"},
    {"login.namePlaceholder", "Letters or digits; must match protocol limits", "英文或数字，长度需符合协议限制"},
    {"login.status", "Status", "状态"},
    {"login.connect", "Connect", "连接"},
    {"login.disconnect", "Disconnect", "断开"},
    {"login.cancel", "Cancel", "取消"},
    {"login.hint", "Closing the window hides it to the system tray; the connection and room stay active.", "关闭主窗口会隐藏到系统托盘，连接与房间会继续保持。"},

    {"lobby.card", "Lobby", "房间大厅"},
    {"lobby.hint", "Select a room to join, or double-click a row.", "选择房间后加入，也可以双击列表项快速加入。"},
    {"lobby.refresh", "Refresh", "刷新"},
    {"lobby.join", "Join selected", "加入选中"},
    {"lobby.leave", "Leave room", "离开房间"},
    {"lobby.col.id", "ID", "ID"},
    {"lobby.col.name", "Name", "名称"},
    {"lobby.col.players", "Players", "人数"},
    {"lobby.col.transport", "Transport", "传输"},
    {"lobby.col.mtu", "MTU", "MTU"},
    {"lobby.passwordPrefix", "[password] ", "[密码] "},
    {"lobby.passwordSuffix", " +password", " +密码"},

    {"create.card", "Create room", "创建房间"},
    {"create.roomName", "Room name", "房间名"},
    {"create.roomNamePlaceholder", "Room name", "输入房间名称"},
    {"create.maxPlayers", "Max players", "最大人数"},
    {"create.roomPassword", "Room password", "房间密码"},
    {"create.roomPasswordPlaceholder", "Optional; leave empty for no room password", "可选，留空则不设置房间密码"},
    {"create.advanced", "Advanced settings", "高级设置"},
    {"create.tcpProtocol", "TCP protocol", "TCP 协议"},
    {"create.tcpFec", "TCP FEC", "TCP FEC"},
    {"create.tcpProfile", "TCP KCP profile", "TCP KCP 模式"},
    {"create.udpProtocol", "UDP protocol", "UDP 协议"},
    {"create.udpFec", "UDP FEC", "UDP FEC"},
    {"create.udpProfile", "UDP KCP profile", "UDP KCP 模式"},
    {"create.mtu", "MTU", "MTU"},
    {"create.create", "Create room", "创建房间"},
    {"create.note", "Room password is only used for join checks. Recommended TCP/UDP defaults are used when advanced settings are hidden.", "房间密码只用于加入校验。隐藏高级设置时使用推荐的 TCP/UDP 默认策略。"},
    {"value.none", "None", "无"},
    {"value.realtime", "Realtime mode", "实时模式"},
    {"value.bulk", "Bulk mode", "大带宽模式"},
    {"value.balancedMtu", "Balanced 1400", "均衡 1400"},
    {"value.aggressiveMtu", "Aggressive 1420", "激进 1420"},
    {"value.safeMtu", "Safe 1280", "稳妥 1280"},

    {"members.title", "Room members", "房间成员"},
    {"members.notJoined", "Not joined", "未加入房间"},
    {"members.myInfo", "My ID: %1  |  Virtual IP: %2", "我的ID: %1  |  虚拟IP: %2"},
    {"members.col.peerId", "PeerID", "PeerID"},
    {"members.col.virtualIp", "Virtual IP", "虚拟IP"},
    {"members.col.name", "Name", "昵称"},
    {"members.col.tcp", "TCP", "TCP"},
    {"members.col.tcpRtt", "TCP RTT", "TCP 延迟"},
    {"members.col.udp", "UDP", "UDP"},
    {"members.col.udpRtt", "UDP RTT", "UDP 延迟"},
    {"members.connecting", "Connecting...", "连接中..."},
    {"members.disconnected", "Disconnected", "未连接"},
    {"members.kcpRealtime", "KCP Realtime", "KCP 实时"},
    {"members.kcpBulk", "KCP Bulk", "KCP 大带宽"},
    {"members.roomNone", "No room", "未加入房间"},
    {"members.roomStatus", "Room %1 / MTU %2", "房间 %1 / MTU %2"},
    {"members.memberCount", "Members %1", "成员 %1"},
    {"members.policySummary", "%1 / FEC %2", "%1 / FEC %2"},
    {"members.detail.title", "Member details", "成员详情"},
    {"members.detail.peerId", "Peer ID", "Peer ID"},
    {"members.detail.virtualIp", "Virtual IP", "虚拟 IP"},
    {"members.detail.name", "Name", "昵称"},
    {"members.detail.tcp", "TCP", "TCP"},
    {"members.detail.tcpRtt", "TCP RTT", "TCP 延迟"},
    {"members.detail.udp", "UDP", "UDP"},
    {"members.detail.udpRtt", "UDP RTT", "UDP 延迟"},
    {"members.detail.room", "Room", "房间"},
    {"members.detail.mtu", "MTU", "MTU"},
    {"members.detail.tcpPolicy", "TCP policy", "TCP 策略"},
    {"members.detail.udpPolicy", "UDP policy", "UDP 策略"},
    {"members.detail.copy", "Copy", "复制"},
    {"members.detail.close", "Close", "关闭"},

    {"logs.title", "Logs", "日志"},
    {"logs.verbose", "Verbose logs", "详细日志"},

    {"settings.card.language", "Language", "语言"},
    {"settings.card.defaults", "Local defaults", "本机默认配置"},
    {"settings.language", "Language", "语言"},
    {"settings.language.english", "English", "英文"},
    {"settings.language.chinese", "中文", "中文"},
    {"settings.server", "Server address", "服务器地址"},
    {"settings.serverPlaceholder", "Server address", "服务器地址"},
    {"settings.port", "Default port", "默认端口"},
    {"settings.name", "Default name", "默认昵称"},
    {"settings.namePlaceholder", "Player name", "玩家昵称"},
    {"settings.verbose", "Verbose logs by default", "默认启用详细日志"},
    {"settings.note", "Server and name are saved locally. Server authentication password and room password are never saved.", "服务器和昵称会保存到本机。服务器鉴权密码和房间密码永不保存。"},

    {"tray.show", "Show window", "显示主窗口"},
    {"tray.connect", "Connect server", "连接服务器"},
    {"tray.disconnect", "Disconnect server", "断开服务器"},
    {"tray.refresh", "Refresh rooms", "刷新房间列表"},
    {"tray.leave", "Leave room", "离开房间"},
    {"tray.quit", "Quit", "退出程序"},
    {"tray.roomConnected", "Room %1 / Connected", "房间 %1 / 已连接"},

    {"dialog.notice", "Notice", "提示"},
    {"dialog.serverAuth.title", "Server authentication", "服务器鉴权"},
    {"dialog.serverAuth.prompt", "Enter server authentication password", "请输入服务器鉴权密码"},
    {"dialog.roomPassword.title", "Room password", "房间密码"},
    {"dialog.roomPassword.prompt", "Enter room password", "请输入房间密码"},

    {"error.enterServer", "Please enter a server address", "请输入服务器地址"},
    {"error.enterName", "Please enter a valid name", "请输入有效昵称"},
    {"error.invalidRoomName", "Invalid room name", "房间名无效"},
    {"error.invalidRoomPassword", "Invalid room password", "房间密码无效"},
    {"error.selectRoom", "Please select a room", "请选择房间"},
    {"error.connectFailed", "Connect failed: %1", "连接失败: %1"},
    {"error.connectTimeout", "Connect timeout (%1 sec)", "连接超时 (%1秒)"},
    {"error.protocolMismatch", "Protocol version mismatch: client=%1 server=%2", "协议版本不匹配: client=%1 server=%2"},
    {"error.invalidServerHello", "Invalid server auth hello", "服务器鉴权握手无效"},
    {"error.invalidAuthResponse", "Invalid server auth response", "服务器鉴权响应无效"},
    {"error.serverProofFailed", "Server auth proof failed", "服务器鉴权证明失败"},
    {"error.streamCorrupt", "TCP stream corrupted, disconnecting", "TCP 流异常，正在断开连接"},
    {"error.connectionDead", "No data received for %1 ms, connection dead", "%1 ms 未收到数据，连接已失效"},

    {"status.connectingServer", "Connecting to server %1:%2 ...", "正在连接服务器 %1:%2 ..."},
    {"status.resolvingHost", "Resolving host %1 ...", "正在解析域名 %1 ..."},
    {"status.resolveFailed", "Host resolve failed: %1", "域名解析失败: %1"},
    {"status.resolvedHost", "Host resolved: %1 -> %2", "域名已解析: %1 -> %2"},
    {"status.connectedLoggingIn", "Connected, logging in...", "已连接，正在登录..."},
    {"status.disconnectedReconnect", "Disconnected from server; retrying in %1 seconds (max %2)%3", "与服务器断开连接，将在 %1 秒后尝试重连 (最多 %2 次)%3"},
    {"status.rejoinSuffix", ", will rejoin room automatically", "，将自动回到房间"},
    {"status.disconnected", "Disconnected from server", "与服务器断开连接"},
    {"status.reconnectFailed", "Auto reconnect failed after %1 attempts", "自动重连失败 (已尝试 %1 次)"},
    {"status.reconnectStoppedResumeWindow", "Auto reconnect stopped after %1 attempts; reconnect manually while the %2-second lease is valid to try restoring the original IP", "自动重连已停止 (已尝试 %1 次)；%2 秒租约有效期内手动连接仍可尝试恢复原虚拟 IP"},
    {"status.reconnectAttempt", "Reconnecting (%1/%2)...", "正在重新连接 (%1/%2)..."},
    {"status.loginSuccess", "Login succeeded, PeerId=%1", "登录成功，PeerId=%1"},
    {"status.reconnectFindingRoom", "Reconnect succeeded, locating room \"%1\"...", "重连成功，正在查找房间 \"%1\"..."},
    {"status.reconnectSuccess", "Reconnect succeeded", "重连成功"},
    {"status.roomCreated", "Room created (ID=%1, IP=%2, MTU=%3)", "房间已创建 (ID=%1, IP=%2, MTU=%3)"},
    {"status.roomJoined", "Joined room (ID=%1, IP=%2, MTU=%3)", "已加入房间 (ID=%1, IP=%2, MTU=%3)"},
    {"status.playerJoined", "Player %1 joined room (IP=%2)", "玩家 %1 加入房间 (IP=%2)"},
    {"status.playerLeft", "Player %1 left room", "玩家 %1 离开房间"},
    {"status.relayReady", "%1 relay connected (TCP=%2, UDP=%3)", "%1 已建立中继连接 (TCP=%2, UDP=%3)"},
    {"status.dataChannelConnected", "Data channel connected", "数据通道已建立"},
    {"status.dataChannelDisconnected", "Data channel disconnected, reconnecting...", "数据通道断开，正在自动重连..."},
    {"status.firewallAdded", "Firewall rule added: allow inbound from 10.10.0.0/24", "防火墙规则已添加：允许 10.10.0.0/24 入站"},
    {"status.firewallAddFailed", "Failed to add firewall rule; game ports may be unreachable", "防火墙规则添加失败，游戏端口可能无法联通"},
    {"status.firewallRemoved", "Firewall rule removed", "防火墙规则已删除"},
    {"status.firewallRemoveFailed", "Failed to remove firewall rule; it may already be absent", "防火墙规则删除失败（可能已不存在）"},
    {"status.tunInitFailed", "Virtual adapter initialization failed (administrator privileges required)", "虚拟网卡初始化失败（需要管理员权限）"},
    {"status.tunIpFailed", "Virtual adapter IP configuration failed", "虚拟网卡IP配置失败"},
    {"status.tunSessionFailed", "Virtual adapter session failed to start", "虚拟网卡会话启动失败"},
    {"status.tunStarted", "Virtual adapter started IP=%1 MTU=%2", "虚拟网卡已启动 IP=%1 MTU=%2"},
    {"status.tcpRelayTimeout", "%1 TCP relay timed out, rebuilding connection...", "%1 TCP中继超时断开，正在尝试重建连接..."},
    {"status.tunnelTimeout", "%1 tunnel timed out, rebuilding connection...", "%1 隧道超时断开，正在尝试重建连接..."},
    {"status.reconnectFoundRoom", "Found room \"%1\" (ID=%2), rejoining...", "找到房间 \"%1\" (ID=%2)，正在重新加入..."},
    {"status.reconnectRoomMissing", "Room \"%1\" no longer exists, recreating...", "房间 \"%1\" 已不存在，正在重新创建..."}
};

} // namespace

void UiStrings::setLanguage(AppLanguage language)
{
    g_language = language;
}

AppLanguage UiStrings::language()
{
    return g_language;
}

QString UiStrings::text(const char* key)
{
    for (size_t i = 0; i < sizeof(kEntries) / sizeof(kEntries[0]); ++i) {
        if (QString::fromLatin1(kEntries[i].key) == QString::fromLatin1(key)) {
            return QString::fromUtf8(g_language == AppLanguage::Chinese
                                     ? kEntries[i].zh : kEntries[i].en);
        }
    }
    return QString::fromLatin1(key);
}

QString UiStrings::languageCode(AppLanguage language)
{
    return language == AppLanguage::Chinese
        ? QStringLiteral("zh")
        : QStringLiteral("en");
}

AppLanguage UiStrings::languageFromCode(const QString& code)
{
    QString normalized = code.trimmed().toLower();
    if (normalized == QStringLiteral("zh") ||
        normalized == QStringLiteral("zh-cn") ||
        normalized == QStringLiteral("chinese")) {
        return AppLanguage::Chinese;
    }
    return AppLanguage::English;
}

} // namespace VLan
