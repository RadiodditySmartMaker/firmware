#pragma once

#include "NodeDB.h"
#include "mesh/generated/meshtastic/mesh.pb.h"
#include <map>
#include <vector>

class ChatHistoryStore
{
  public:
    ChatHistoryStore();

    void loadFromDisk();

    // 频道消息历史：按 channel index 保存最近的广播消息。
    bool isMeshPacketListEmpty(uint8_t channel) const;
    void saveMeshPacket(const meshtastic_MeshPacket &mp);
    meshtastic_MeshPacket getRecentMeshPacket(uint8_t channel, uint8_t recentIndex) const;
    int getMeshPacketListSize(uint8_t channel) const;

    void restoreChannelPackets();
    void restoreDirectMessages();
    void deleteCurrentChannelMessage(uint8_t channelIndex, uint16_t packetIndex);
    void deleteAllChannelMessagesForChannel(uint8_t channelIndex);

    // 私信历史：按对端节点号保存最近的 direct message。
    bool isDirectMessageListEmpty() const;
    bool isDirectMessageListEmptyForNode(NodeNum nodeNum) const;
    meshtastic_MeshPacket getRecentDirectMessage(uint8_t recentIndex) const;
    int getDirectMessageListSize() const;
    int getDirectMessageListSizeForNode(NodeNum nodeNum) const;
    void setCurrentDirectMessageNode(NodeNum nodeNum);
    NodeNum getCurrentDirectMessageNode() const { return currentDirectMessageNode; }
    uint8_t getCurrentDirectMessageIndex() const { return currentDirectMessageIndex; }
    void setCurrentDirectMessageIndex(uint8_t index) { currentDirectMessageIndex = index; }
    std::vector<NodeNum> getDirectMessageNodeList() const;
    int getDirectMessageNodeCount() const;
    void deleteCurrentDirectMessage();
    void deleteAllDirectMessagesForNode(NodeNum nodeNum);

    // 将内存中未落盘的历史写入 flash（关机/睡眠/重启前调用）。
    void persistToDisk();

  private:
    static constexpr uint8_t kMaxChannels = 8;
    static constexpr uint8_t kChannelMessageCapacity = 10;
    static constexpr uint8_t kDirectMessageCapacity = 10;

    // channelPackets 使用真实 channel index(0~7) 作为数组索引。
    std::vector<meshtastic_MeshPacket> channelPackets[kMaxChannels];
    // directMessagesByNode 的 key 是聊天对端节点，不区分消息收/发方向。
    std::map<NodeNum, std::vector<meshtastic_MeshPacket>> directMessagesByNode;
    NodeNum currentDirectMessageNode = 0;
    uint8_t currentDirectMessageIndex = 0;

    uint8_t dirtyChannelMask = 0;
    std::vector<NodeNum> dirtyDmNodes;

    void pushChannelPacket(uint8_t channelIndex, const meshtastic_MeshPacket &mp);
    void pushDirectMessage(const meshtastic_MeshPacket &mp);
    void markChannelDirty(uint8_t channelIndex);
    void markDmNodeDirty(NodeNum nodeNum);
    void clearChannelDirty(uint8_t channelIndex);
    void clearDmNodeDirty(NodeNum nodeNum);
    void persistChannelToDisk(uint8_t channelIndex);
    void persistDirectMessagesForNodeToDisk(NodeNum nodeNum);
};

extern ChatHistoryStore *chatHistoryStore;
