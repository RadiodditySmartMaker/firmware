#include "ChatHistoryStore.h"
#include "DebugConfiguration.h"
#include "FSCommon.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "mesh-pb-constants.h"
#include "meshUtils.h"
#include <cstring>
#include <pb_decode.h>
#include <pb_encode.h>

#ifdef Nodara

namespace
{
constexpr uint8_t kHistoryFileVersion = 1;
constexpr uint8_t kLegacyCapacity = 10;

// 单条消息 protobuf 编码缓冲（静态，避免启动恢复时栈溢出）
static uint8_t packetEncodeBuf[meshtastic_MeshPacket_size];

void channelHistoryPath(uint8_t channelIndex, char *buf, size_t bufLen)
{
    snprintf(buf, bufLen, "/prefs/ch%02x_hist.bin", channelIndex);
}

void dmHistoryPath(NodeNum nodeNum, char *buf, size_t bufLen)
{
    snprintf(buf, bufLen, "/prefs/dm_%08x_hist.bin", nodeNum);
}

bool encodeMeshPacket(const meshtastic_MeshPacket &mp, uint8_t *outBuf, size_t outBufSize, size_t &encodedLen)
{
    pb_ostream_t stream = pb_ostream_from_buffer(outBuf, outBufSize);
    if (!pb_encode(&stream, meshtastic_MeshPacket_fields, &mp))
        return false;
    encodedLen = stream.bytes_written;
    return true;
}

bool decodeMeshPacket(const uint8_t *inBuf, size_t inLen, meshtastic_MeshPacket &mp)
{
    pb_istream_t stream = pb_istream_from_buffer(inBuf, inLen);
    return pb_decode(&stream, meshtastic_MeshPacket_fields, &mp);
}

bool ensurePrefsDir()
{
#ifdef FSCom
    spiLock->lock();
    bool ok = FSCom.mkdir("/prefs");
    spiLock->unlock();
    return ok;
#else
    return false;
#endif
}

bool writePacketHistoryFile(const char *path, const std::vector<meshtastic_MeshPacket> &messages, uint8_t maxMessages)
{
#ifdef FSCom
    if (!ensurePrefsDir())
        return false;

    if (messages.empty()) {
        concurrency::LockGuard guard(spiLock);
        if (FSCom.exists(path))
            FSCom.remove(path);
        return true;
    }

    auto f = SafeFile(path, false);
    uint8_t count = messages.size() > maxMessages ? maxMessages : (uint8_t)messages.size();

    spiLock->lock();
    f.write(kHistoryFileVersion);
    f.write(count);

    for (uint8_t i = 0; i < count; ++i) {
        size_t encodedLen = 0;
        if (!encodeMeshPacket(messages.at(i), packetEncodeBuf, sizeof(packetEncodeBuf), encodedLen)) {
            spiLock->unlock();
            LOG_WARN("ChatHistoryStore: encode failed for %s index %u", path, (unsigned)i);
            return false;
        }
        uint16_t leLen = (uint16_t)encodedLen;
        f.write((uint8_t)(leLen & 0xff));
        f.write((uint8_t)(leLen >> 8));
        f.write(packetEncodeBuf, encodedLen);
    }
    spiLock->unlock();

    if (!f.close()) {
        LOG_WARN("ChatHistoryStore: failed to close %s", path);
        return false;
    }
    LOG_DEBUG("ChatHistoryStore: saved %u messages to %s", count, path);
    return true;
#else
    return false;
#endif
}

// 将历史文件读入 dest；返回成功加载的条数（0 表示失败或空文件）
uint8_t loadPacketHistoryInto(std::vector<meshtastic_MeshPacket> &dest, const char *path, uint8_t maxMessages)
{
#ifdef FSCom
    dest.clear();

    concurrency::LockGuard guard(spiLock);
    if (!FSCom.exists(path))
        return 0;

    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return 0;

    const size_t fileSize = f.size();
    if (fileSize < 2) {
        f.close();
        return 0;
    }

    uint8_t version = 0;
    if (f.readBytes((char *)&version, 1) != 1 || version != kHistoryFileVersion) {
        f.close();
        return 0;
    }

    uint8_t count = 0;
    if (f.readBytes((char *)&count, 1) != 1) {
        f.close();
        return 0;
    }
    if (count > maxMessages)
        count = maxMessages;

    uint8_t loaded = 0;
    for (uint8_t i = 0; i < count; ++i) {
        uint8_t lenBytes[2];
        if (f.readBytes((char *)lenBytes, 2) != 2)
            break;

        uint16_t encodedLen = (uint16_t)lenBytes[0] | ((uint16_t)lenBytes[1] << 8);
        if (encodedLen == 0 || encodedLen > sizeof(packetEncodeBuf))
            break;

        if (f.readBytes((char *)packetEncodeBuf, encodedLen) != encodedLen)
            break;

        meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
        if (!decodeMeshPacket(packetEncodeBuf, encodedLen, mp))
            continue;

        if (dest.size() >= maxMessages)
            dest.erase(dest.begin());
        dest.push_back(mp);
        loaded++;
    }

    f.close();
    return loaded;
#else
    return 0;
#endif
}

bool loadLegacyMeshPacketFile(const char *path, meshtastic_MeshPacket &mp)
{
#ifdef FSCom
    mp = meshtastic_MeshPacket_init_zero;
    concurrency::LockGuard guard(spiLock);
    if (!FSCom.exists(path))
        return false;

    auto f = FSCom.open(path, FILE_O_READ);
    if (!f)
        return false;

    pb_istream_t stream = {&readcb, &f, meshtastic_MeshPacket_size};
    const bool ok = pb_decode(&stream, meshtastic_MeshPacket_fields, &mp);
    f.close();
    return ok;
#else
    return false;
#endif
}

void deleteLegacyChannelPacketFiles(uint8_t channelIndex)
{
#ifdef FSCom
    concurrency::LockGuard guard(spiLock);
    for (uint8_t i = 0; i < kLegacyCapacity; ++i) {
        char legacyPath[64];
        snprintf(legacyPath, sizeof(legacyPath), "/prefs/channel%02x_packet%02x.proto", channelIndex, i);
        if (FSCom.exists(legacyPath))
            FSCom.remove(legacyPath);
    }
#endif
}

void deleteLegacyDmPacketFiles(NodeNum nodeNum)
{
#ifdef FSCom
    concurrency::LockGuard guard(spiLock);
    for (uint8_t i = 0; i < kLegacyCapacity; ++i) {
        char legacyPath[64];
        snprintf(legacyPath, sizeof(legacyPath), "/prefs/dm_node%08x_msg%02x.proto", nodeNum, i);
        if (FSCom.exists(legacyPath))
            FSCom.remove(legacyPath);
    }
#endif
}

bool restoreLegacyChannelPackets(uint8_t channelIndex, std::vector<meshtastic_MeshPacket> &messages)
{
    messages.clear();
#ifdef FSCom
    bool found = false;
    for (uint8_t j = 0; j < kLegacyCapacity; ++j) {
        char legacyPath[64];
        snprintf(legacyPath, sizeof(legacyPath), "/prefs/channel%02x_packet%02x.proto", channelIndex, j);
        meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
        if (loadLegacyMeshPacketFile(legacyPath, mp)) {
            messages.push_back(mp);
            found = true;
        }
    }
    return found;
#else
    return false;
#endif
}

bool restoreLegacyDirectMessages(NodeNum nodeNum, std::vector<meshtastic_MeshPacket> &messages)
{
    messages.clear();
#ifdef FSCom
    bool found = false;
    for (uint8_t j = 0; j < kLegacyCapacity; ++j) {
        char legacyPath[64];
        snprintf(legacyPath, sizeof(legacyPath), "/prefs/dm_node%08x_msg%02x.proto", nodeNum, j);
        meshtastic_MeshPacket mp = meshtastic_MeshPacket_init_zero;
        if (loadLegacyMeshPacketFile(legacyPath, mp)) {
            messages.push_back(mp);
            found = true;
        }
    }
    return found;
#else
    return false;
#endif
}

} // namespace

ChatHistoryStore *chatHistoryStore = nullptr;

ChatHistoryStore::ChatHistoryStore() {}

void ChatHistoryStore::loadFromDisk()
{
    restoreChannelPackets();
    restoreDirectMessages();
}

bool ChatHistoryStore::isMeshPacketListEmpty(uint8_t channel) const
{
    if (channel >= kMaxChannels) {
        LOG_INFO("ChatHistoryStore: incorrect channel num = 0x%x!", channel);
        return false;
    }

    return channelPackets[channel].empty();
}

void ChatHistoryStore::pushChannelPacket(uint8_t channelIndex, const meshtastic_MeshPacket &mp)
{
    if (channelIndex >= kMaxChannels)
        return;

    // 固定容量环形语义：满了就丢弃最旧的一条，保留最近消息。
    if (channelPackets[channelIndex].size() >= kChannelMessageCapacity)
        channelPackets[channelIndex].erase(channelPackets[channelIndex].begin());

    channelPackets[channelIndex].push_back(mp);
}

void ChatHistoryStore::markChannelDirty(uint8_t channelIndex)
{
    if (channelIndex < kMaxChannels)
        dirtyChannelMask |= (1u << channelIndex);
}

void ChatHistoryStore::markDmNodeDirty(NodeNum nodeNum)
{
    if (nodeNum == 0)
        return;
    for (NodeNum n : dirtyDmNodes) {
        if (n == nodeNum)
            return;
    }
    dirtyDmNodes.push_back(nodeNum);
}

void ChatHistoryStore::clearChannelDirty(uint8_t channelIndex)
{
    if (channelIndex < kMaxChannels)
        dirtyChannelMask &= ~(1u << channelIndex);
}

void ChatHistoryStore::clearDmNodeDirty(NodeNum nodeNum)
{
    for (auto it = dirtyDmNodes.begin(); it != dirtyDmNodes.end(); ++it) {
        if (*it == nodeNum) {
            dirtyDmNodes.erase(it);
            return;
        }
    }
}

void ChatHistoryStore::persistChannelToDisk(uint8_t channelIndex)
{
    if (channelIndex >= kMaxChannels)
        return;

    char path[64];
    channelHistoryPath(channelIndex, path, sizeof(path));
    writePacketHistoryFile(path, channelPackets[channelIndex], kChannelMessageCapacity);
    deleteLegacyChannelPacketFiles(channelIndex);
}

void ChatHistoryStore::persistDirectMessagesForNodeToDisk(NodeNum nodeNum)
{
    if (nodeNum == 0)
        return;

    char path[64];
    dmHistoryPath(nodeNum, path, sizeof(path));

    auto it = directMessagesByNode.find(nodeNum);
    if (it == directMessagesByNode.end() || it->second.empty())
        writePacketHistoryFile(path, {}, kDirectMessageCapacity);
    else
        writePacketHistoryFile(path, it->second, kDirectMessageCapacity);

    deleteLegacyDmPacketFiles(nodeNum);
}

void ChatHistoryStore::persistToDisk()
{
    for (uint8_t channelIndex = 0; channelIndex < kMaxChannels; ++channelIndex) {
        if (dirtyChannelMask & (1u << channelIndex))
            persistChannelToDisk(channelIndex);
    }
    dirtyChannelMask = 0;

    for (NodeNum nodeNum : dirtyDmNodes)
        persistDirectMessagesForNodeToDisk(nodeNum);
    dirtyDmNodes.clear();
}

void ChatHistoryStore::saveMeshPacket(const meshtastic_MeshPacket &mp)
{
    if (mp.channel >= kMaxChannels) {
        LOG_INFO("ChatHistoryStore: incorrect mp.channel = 0x%x!", mp.channel);
        return;
    }

    // direct message 使用私信列表保存；广播消息按频道保存。
    // 收包路径只更新 RAM，落盘推迟到 persistToDisk()（关机/睡眠/重启）。
    if (mp.to != NODENUM_BROADCAST) {
        pushDirectMessage(mp);
        return;
    }

    pushChannelPacket(mp.channel, mp);
    markChannelDirty(mp.channel);
}

void ChatHistoryStore::restoreChannelPackets()
{
    for (uint8_t i = 0; i < kMaxChannels; ++i) {
        if (channelFile.channels[i].role != meshtastic_Channel_Role_PRIMARY &&
            channelFile.channels[i].role != meshtastic_Channel_Role_SECONDARY)
            continue;

        char path[64];
        channelHistoryPath(i, path, sizeof(path));

        uint8_t loaded = loadPacketHistoryInto(channelPackets[i], path, kChannelMessageCapacity);
        if (loaded > 0) {
            LOG_INFO("restored %u messages for channel %u", (unsigned)loaded, (unsigned)i);
            continue;
        }

        std::vector<meshtastic_MeshPacket> legacy;
        if (restoreLegacyChannelPackets(i, legacy)) {
            channelPackets[i].clear();
            for (const auto &mp : legacy)
                pushChannelPacket(i, mp);
            writePacketHistoryFile(path, channelPackets[i], kChannelMessageCapacity);
            deleteLegacyChannelPacketFiles(i);
            LOG_INFO("migrated %u legacy messages for channel %u", (unsigned)legacy.size(), (unsigned)i);
        }
    }
}

void ChatHistoryStore::restoreDirectMessages()
{
    // 私信文件名包含对端节点号，因此需要先遍历已知节点再尝试恢复。
    for (size_t i = 0; i < nodeDB->getNumMeshNodes(); ++i) {
        meshtastic_NodeInfoLite *node = nodeDB->getMeshNodeByIndex(i);
        if (!node)
            continue;

        NodeNum nodeNum = node->num;
        auto &nodeMessages = directMessagesByNode[nodeNum];
        nodeMessages.clear();

        char path[64];
        dmHistoryPath(nodeNum, path, sizeof(path));

        uint8_t loaded = loadPacketHistoryInto(nodeMessages, path, kDirectMessageCapacity);
        if (loaded > 0) {
            LOG_INFO("restored %u DM(s) for node 0x%08x", (unsigned)loaded, nodeNum);
        } else if (restoreLegacyDirectMessages(nodeNum, nodeMessages)) {
            writePacketHistoryFile(path, nodeMessages, kDirectMessageCapacity);
            deleteLegacyDmPacketFiles(nodeNum);
            LOG_INFO("migrated %u legacy DM(s) for node 0x%08x", (unsigned)nodeMessages.size(), nodeNum);
        }
    }

    if (currentDirectMessageNode != 0)
        return;

    // 默认选中第一个有历史的对端节点（跳过本机），进入 DM 页面时能直接显示最近会话。
    const NodeNum selfNum = nodeDB->getNodeNum();
    bool foundNodeWithMessages = false;
    for (const auto &pair : directMessagesByNode) {
        if (pair.first == selfNum)
            continue;
        if (!pair.second.empty()) {
            currentDirectMessageNode = pair.first;
            currentDirectMessageIndex = pair.second.size() - 1;
            LOG_INFO("Auto-selected first node with messages: 0x%08x", currentDirectMessageNode);
            foundNodeWithMessages = true;
            break;
        }
    }

    if (!foundNodeWithMessages) {
        for (size_t i = 0; i < nodeDB->getNumMeshNodes(); ++i) {
            meshtastic_NodeInfoLite *peer = nodeDB->getMeshNodeByIndex(i);
            if (!peer || peer->num == selfNum)
                continue;
            currentDirectMessageNode = peer->num;
            currentDirectMessageIndex = 0;
            LOG_INFO("No nodes with messages, auto-selected first peer: 0x%08x", currentDirectMessageNode);
            break;
        }
    }
}

meshtastic_MeshPacket ChatHistoryStore::getRecentMeshPacket(uint8_t channel, uint8_t recentIndex) const
{
    if ((channel >= kMaxChannels) || (recentIndex >= channelPackets[channel].size())) {
        LOG_INFO("ChatHistoryStore: invalid argument, channel num = 0x%x, recent_index = %d!", channel, recentIndex);
        meshtastic_MeshPacket emptyPacket;
        memset(&emptyPacket, 0, sizeof(emptyPacket));
        return emptyPacket;
    }

    return channelPackets[channel].at(recentIndex);
}

int ChatHistoryStore::getMeshPacketListSize(uint8_t channel) const
{
    if (channel >= kMaxChannels)
        return 0;
    return channelPackets[channel].size();
}

void ChatHistoryStore::pushDirectMessage(const meshtastic_MeshPacket &mp)
{
    NodeNum otherNode;
    NodeNum myNodeNum = nodeDB->getNodeNum();
    LOG_DEBUG("myNodeNum: 0x%08x", myNodeNum);
    LOG_DEBUG("getFrom:0x%08x", getFrom(&mp));

    // 私信列表按“对端节点”聚合：我发出的消息取 mp.to，收到的消息取 from。
    if (getFrom(&mp) == myNodeNum)
        otherNode = mp.to;
    else
        otherNode = getFrom(&mp);

    if (otherNode == 0 || otherNode == NODENUM_BROADCAST)
        return;

    auto &nodeMessages = directMessagesByNode[otherNode];
    // 每个对端只保留最近 kDirectMessageCapacity 条，避免长期占用过多 RAM/文件。
    if (nodeMessages.size() >= kDirectMessageCapacity)
        nodeMessages.erase(nodeMessages.begin());

    nodeMessages.push_back(mp);
    markDmNodeDirty(otherNode);

    if (currentDirectMessageNode == 0) {
        currentDirectMessageNode = otherNode;
        currentDirectMessageIndex = nodeMessages.size() - 1;
    } else if (currentDirectMessageNode == otherNode) {
        currentDirectMessageIndex = nodeMessages.size() - 1;
    }
}

bool ChatHistoryStore::isDirectMessageListEmpty() const
{
    return directMessagesByNode.empty();
}

bool ChatHistoryStore::isDirectMessageListEmptyForNode(NodeNum nodeNum) const
{
    auto it = directMessagesByNode.find(nodeNum);
    return (it == directMessagesByNode.end() || it->second.empty());
}

meshtastic_MeshPacket ChatHistoryStore::getRecentDirectMessage(uint8_t recentIndex) const
{
    if (currentDirectMessageNode == 0) {
        meshtastic_MeshPacket emptyPacket;
        memset(&emptyPacket, 0, sizeof(emptyPacket));
        return emptyPacket;
    }

    auto it = directMessagesByNode.find(currentDirectMessageNode);
    if (it == directMessagesByNode.end() || recentIndex >= it->second.size()) {
        meshtastic_MeshPacket emptyPacket;
        memset(&emptyPacket, 0, sizeof(emptyPacket));
        return emptyPacket;
    }

    return it->second.at(recentIndex);
}

int ChatHistoryStore::getDirectMessageListSize() const
{
    if (currentDirectMessageNode == 0)
        return 0;

    auto it = directMessagesByNode.find(currentDirectMessageNode);
    if (it == directMessagesByNode.end())
        return 0;

    return it->second.size();
}

int ChatHistoryStore::getDirectMessageListSizeForNode(NodeNum nodeNum) const
{
    auto it = directMessagesByNode.find(nodeNum);
    if (it == directMessagesByNode.end())
        return 0;
    return it->second.size();
}

void ChatHistoryStore::setCurrentDirectMessageNode(NodeNum nodeNum)
{
    currentDirectMessageNode = nodeNum;
    auto it = directMessagesByNode.find(nodeNum);
    // 切换会话时默认定位到最新一条，UP/DOWN 再从这里开始浏览。
    if (it != directMessagesByNode.end() && !it->second.empty())
        currentDirectMessageIndex = it->second.size() - 1;
    else
        currentDirectMessageIndex = 0;
}

std::vector<NodeNum> ChatHistoryStore::getDirectMessageNodeList() const
{
    std::vector<NodeNum> nodeList;
    for (const auto &pair : directMessagesByNode) {
        if (!pair.second.empty())
            nodeList.push_back(pair.first);
    }
    return nodeList;
}

int ChatHistoryStore::getDirectMessageNodeCount() const
{
    int count = 0;
    for (const auto &pair : directMessagesByNode) {
        if (!pair.second.empty())
            count++;
    }
    return count;
}

void ChatHistoryStore::deleteCurrentDirectMessage()
{
    if (currentDirectMessageNode == 0)
        return;

    auto it = directMessagesByNode.find(currentDirectMessageNode);
    if (it == directMessagesByNode.end() || it->second.empty())
        return;

    auto &nodeMessages = it->second;
    if (currentDirectMessageIndex >= nodeMessages.size())
        return;

    nodeMessages.erase(nodeMessages.begin() + currentDirectMessageIndex);
    persistDirectMessagesForNodeToDisk(currentDirectMessageNode);

    if (currentDirectMessageIndex >= nodeMessages.size() && !nodeMessages.empty())
        currentDirectMessageIndex = nodeMessages.size() - 1;
    else if (nodeMessages.empty())
        currentDirectMessageIndex = 0;

    clearDmNodeDirty(currentDirectMessageNode);
    LOG_INFO("Deleted direct message at index %d for node 0x%08x", currentDirectMessageIndex, currentDirectMessageNode);
}

void ChatHistoryStore::deleteAllDirectMessagesForNode(NodeNum nodeNum)
{
    if (nodeNum == 0)
        return;

    auto it = directMessagesByNode.find(nodeNum);
    if (it == directMessagesByNode.end())
        return;

    char path[64];
    dmHistoryPath(nodeNum, path, sizeof(path));
    writePacketHistoryFile(path, {}, kDirectMessageCapacity);
    deleteLegacyDmPacketFiles(nodeNum);
    directMessagesByNode.erase(it);

    if (currentDirectMessageNode == nodeNum) {
        currentDirectMessageNode = 0;
        currentDirectMessageIndex = 0;
    }

    clearDmNodeDirty(nodeNum);
    LOG_INFO("Deleted all direct messages for node 0x%08x", nodeNum);
}

void ChatHistoryStore::deleteCurrentChannelMessage(uint8_t channelIndex, uint16_t packetIndex)
{
    if (channelIndex >= kMaxChannels)
        return;

    auto &channelMessages = channelPackets[channelIndex];
    if (packetIndex >= channelMessages.size()) {
        LOG_WARN("deleteCurrentChannelMessage: packet_index %d >= size %u", packetIndex, (unsigned)channelMessages.size());
        return;
    }

    LOG_INFO("Deleting channel message: channel=%d, index=%d, total=%u", channelIndex, packetIndex,
             (unsigned)channelMessages.size());

    channelMessages.erase(channelMessages.begin() + packetIndex);
    persistChannelToDisk(channelIndex);
    LOG_INFO("After erase: channel %d has %u messages", channelIndex, (unsigned)channelMessages.size());

    clearChannelDirty(channelIndex);
    LOG_INFO("Deleted channel message at index %d for channel %d, remaining %u messages", packetIndex, channelIndex,
             (unsigned)channelMessages.size());
}

void ChatHistoryStore::deleteAllChannelMessagesForChannel(uint8_t channelIndex)
{
    if (channelIndex >= kMaxChannels)
        return;

    char path[64];
    channelHistoryPath(channelIndex, path, sizeof(path));
    writePacketHistoryFile(path, {}, kChannelMessageCapacity);
    deleteLegacyChannelPacketFiles(channelIndex);
    channelPackets[channelIndex].clear();
    clearChannelDirty(channelIndex);
    LOG_INFO("Deleted all channel messages for channel %d", channelIndex);
}

#endif
