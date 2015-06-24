#ifndef __CLUSTER_H_FFS
#define __CLUSTER_H_FFS

#include <cstdio>

#include "f_types.h"
#include "lru.hpp"
#include "SparseArray.h"

// End Of Cluster
#define EOC ((cluster_t)0xffffffff)
#define MAX_STACK 512

#define CLUSTER_4K 4096

// �����ض��壬��Щ���ǲ��ܱ��ͷŵ��ģ��������ض���;
// ��Щ������ӵ��ȷ���Ĵ���ţ������ڴ����������Ϳ��Է���
// ���ڱ�ϵͳ���Ϊ��̬���䵽�Ĵص�������Ȳ���֪��Ϊ���ܹ�
// ���ϲ�Ӧ���ܹ���ȷ����λ�ö�ȡ����һ������ݣ��ʱ�����3���ء�
// ���� MM ���� MMC���Ǵع���ϵͳ������Ŵط�����Ϣ�ģ��ϲ�Ӧ��
// ��Ӧ�÷��ʱ��ء�
// PRIMARY �� SECONDARY ������ʹ�á������������б����ص�Ŀ����
// ���ڽ���������ơ�
#define CLUSTER_REV_MM 0
#define CLUSTER_REV_PRIMARY 1
#define CLUSTER_REV_SECONDARY 2

#define CLUSTER_REV_MAX CLUSTER_REV_SECONDARY
#define CLUSTER_REV_COUNT (CLUSTER_REV_MAX + 1)

typedef struct
{
    cluster_size_t cluster_size; // �ش�С
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
    byte_t* buffer;
    unsigned __int16 flag;
    unsigned __int32 ref;

    ClusterContainer(ClusterMgr* mgr, cluster_t cluster);
    void NewRef();
    bool ReleaseRef(); // �����Ƿ� free

    bool Modify();
public:
    ~ClusterContainer();
    bool Sync();
    bool Avaliable();
    cluster_size_t Read(cluster_size_t dst_offset, cluster_size_t src_offset, cluster_size_t count, byte_t* dst);
    cluster_size_t Write(cluster_size_t dst_offset, cluster_size_t src_offset, cluster_size_t count, byte_t* src);
    cluster_size_t Memset(cluster_size_t dst_offset, cluster_size_t count, byte_t value);

    cluster_t GetCluster();
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

    cluster_size_t GetClusterSize(); // ��ȡ�ش�С
    cluster_t GetFreeCluster(); // ��ȡ���д�����

    cluster_t* Allocate(cluster_t count, cluster_t* out); // ���� count ���أ�������out�У�����ɹ�������out�����򷵻�null
    bool Free(cluster_t cluster); // �ͷ�һ�� cluster

    ClusterContainer* Fetch(cluster_t cluster); // �����Ϊ cluster �Ĵص����ݼ��ص��ڴ���
    bool Dispose(ClusterContainer& cluster); // �ͷ� cluster �����õ����ݣ���ʱ���Խ��ڴ��е�����д�ص�����

    bool Sync(); // �����й�����޸�ȫ��д�����

    bool IsActive(cluster_t id);
};

#endif
