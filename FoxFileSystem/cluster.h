#ifndef __CLUSTER_H
#define __CLUSTER_H

#include <cstdio>

#include "lru.hpp"
#include "SparseArray.h"

typedef unsigned __int32 cluster_t;
// End Of Cluster
#define EOC ((cluster_t)0xffffffff)
#define MAX_STACK 512

#define CLUSTER_4K 4096

typedef struct
{
    size_t cluster_size; // �ش�С
    cluster_t cluster_count; // ������
    cluster_t free_cluster_count; // ���д�����
} ClusterInfo;

typedef struct
{
    cluster_t count;
    cluster_t next_stack;
    cluster_t stack[MAX_STACK];
} FreeStack;

#define MASK_FALG 0xF000
#define MASK_DCOUNT 0x0FFF // dirty ����

#define FLAG_AVAILABLE 0x8000
#define FLAG_DIRTY 0x1000

#define DIRTY_MAX 1024

class ClusterContainer
{
private:
    friend class ClusterMgr;

    ClusterMgr* cluster_mgr;
    FILE* fd;
    ClusterInfo* info;

    cluster_t cluster;
    unsigned __int8* buffer;
    unsigned __int16 flag;
    unsigned __int32 ref;
    
    ClusterContainer(ClusterMgr* mgr, cluster_t cluster);
    ~ClusterContainer();
    bool Sync();
    void NewRef();
    bool ReleaseRef(); // �����Ƿ� free
public:
    bool Avaliable();
    size_t Read(size_t dst_offset, size_t src_offset, size_t count, unsigned __int8* dst);
    size_t Write(size_t dst_offset, size_t src_offset, size_t count, unsigned __int8* src);
};

class ClusterMgr
{
private:
    friend class ClusterContainer;

    bool opened;
    FILE* fd;
    ClusterInfo info;
    ClusterContainer* MMC; // Master Meta Cluster ��Ԫ���ݴ�

    LruCache<cluster_t, ClusterContainer*>* inactive_cache; // �ǻ���棬LRU
    SparseArray<cluster_t, ClusterContainer*>* active_cache; // �����

    ClusterContainer* _Fetch(cluster_t cluster);
public:
    ClusterMgr();
    ~ClusterMgr();

    static bool CreatePartition(const char* file, ClusterInfo* info); // ���������ļ�
    bool LoadPartition(const char* file); // ��������ļ�
    bool ClosePartition(); // �رշ����ļ�

    size_t GetClusterSize(); // ��ȡ�ش�С
    cluster_t GetFreeCluster(); // ��ȡ���д�����

    cluster_t* Allocate(cluster_t count, cluster_t* out); // ���� count ���أ�������out�У�����ɹ�������out�����򷵻�null
    bool Free(cluster_t cluster); // �ͷ�һ�� cluster

    ClusterContainer* Fetch(cluster_t cluster); // �����Ϊ cluster �Ĵص����ݼ��ص��ڴ���
    bool Dispose(ClusterContainer& cluster); // �ͷ� cluster �����õ����ݣ���ʱ���Խ��ڴ��е�����д�ص�����

    bool Sync(); // �����й�����޸�ȫ��д�����
};

#endif
