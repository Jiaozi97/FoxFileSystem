#ifndef __NODE_H
#define __NODE_H

#include "cluster.h"

#define MAX_DIRECT 12
#define MAX_INDIRECT 3

#define MODE_MASK_TYPE 0xF000
#define MODE_MASK_USER 0xF00
#define MODE_MASK_GROUP 0xF0
#define MODE_MASK_OTHER 0xF

#define TYPE_DIR 0x0000
#define TYPE_NORMAL 0x1000

typedef struct
{
    unsigned __int16 flag;
    unsigned __int16 mode;
    unsigned __int32 time_create;
    unsigned __int32 time_modify;
    unsigned __int32 time_visit;
    unsigned __int32 size;
    cluster_t cluster_count; // ��ʱû���õ����ֶ�
    cluster_t index_direct[MAX_DIRECT];
    cluster_t index_indir[MAX_INDIRECT];
    __int32 reserved[11]; // padding to 128 byte
    //unsigned __int8 data[0]; // �ļ����С�� �سߴ� - 128B �Ĳ���ֱ�ӷ���������
} INode;

typedef struct
{
    size_t no_index;
    size_t index_direct[MAX_DIRECT];
    size_t index_indir[MAX_INDIRECT];

    cluster_t index_per_cluster; // ÿ���ؿɴ�����ٸ�����

    size_t size_level[MAX_INDIRECT + 1]; // �������������ɵĳߴ�
} CIndex; // ������¼ÿһ�������ֱ���Ա�������ֽ����ݵĽṹ��

class Node
{
private:
    friend class NodeMgr;

    NodeMgr* node_mgr;
    ClusterContainer* MC; // Main Cluster
    INode inode;
    size_t cdo; // current data offset����ǰ����ָ��λ��

    size_t seek_index[MAX_INDIRECT + 1]; // ClusterSeek����������� �༶����·��
    size_t seek_cluster_offset; // ClusterSeek����������� ����ƫ��

    ClusterContainer* target_cluster;
    size_t target_offset;
    size_t target_cluster_offset;

    ClusterContainer* current; // ��ǰ���ݴ�
    size_t cco; // current cluster offset����ǰ����ƫ��

    unsigned __int16 flag;
    CIndex index_boundary;

    Node(NodeMgr* mgr, ClusterContainer* mc);
    bool Init();
    bool Load();
    bool Modify();

    __int8 ClusterSeek(size_t offset); // ����offset��Ӧ��ƫ�������ĸ�����
    bool _LoadCluster(size_t offset);
    ClusterContainer* LoadClusterByIndex(__int8 depth, size_t* index);
    bool LoadCluster(size_t offset);

    bool Shrink(size_t curr, size_t target);
    bool Expand(size_t curr, size_t target);

    bool RemoveClusterRight(__int8 depth, cluster_t index, size_t* edge_right);
    bool RemoveClusterLeft(__int8 depth, cluster_t index, size_t* edge_left);
    bool RemoveCluster(__int8 depth, cluster_t index, size_t* edge_left, size_t* edge_right);

    cluster_t BuildClusterRight(__int8 depth, size_t* edge_right);
    bool BuildClusterLeft(__int8 depth, cluster_t index, size_t* edge_left);
    bool BuildCluster(__int8 depth, cluster_t index, size_t* edge_left, size_t* edge_right);
public:
    cluster_t GetNodeId();

    bool Sync();

    unsigned __int16 GetMode(unsigned __int16 mask);
    bool SetMode(unsigned __int16 mode, unsigned __int16 mask);

    size_t GetSize();
    bool Truncate(size_t size); // �ı� Node ��С

    size_t GetPointer(); // ��õ�ǰ����ָ��λ��
    __int64 Seek(size_t offset); // �ı䵱ǰ����ָ��
    size_t Write(void const* buffer, size_t size);
    size_t Read(void* buffer, size_t size);
};

class NodeMgr
{
private:
    friend class Node;

    ClusterMgr* cluster_service;

    CIndex index_boundary;

    Node* CreateNode(ClusterContainer* mc);

public:
    NodeMgr(ClusterMgr* cluster_mgr);

    Node* CreateRootNode();
    Node* OpenRootNode();

    Node* CreateNode();
    Node* OpenNode(cluster_t node);
    bool Close(Node* node); // �رո�Node��д��ȫ���޸ĵ�����
    bool Delete(Node* node); // �رղ�ɾ����Node

    bool Sync();
};

#endif
