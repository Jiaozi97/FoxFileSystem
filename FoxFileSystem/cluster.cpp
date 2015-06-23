#include <cstring>

#include "cluster.h"
#include "util.h"

ClusterMgr::ClusterMgr() :
    opened(false),
    fd(NULL),
    MMC(NULL),
    inactive_cache(NULL),
    active_cache(NULL)
{
}

ClusterMgr::~ClusterMgr()
{

}

bool ClusterMgr::CreatePartition(const char* file, ClusterInfo* info)
{
    FILE* fp = fopen(file, "wb+");
    if (fp == NULL)
    {
        return false;
    }

    long total_size = info->cluster_count * info->cluster_size;

    ASSERT_EOF(fseek(fp, total_size - 1, SEEK_SET));
    ASSERT_EOF(fputc(0, fp));

    info->free_cluster_count = info->cluster_count - CLUSTER_REV_COUNT;
    // д�� MMC
    ASSERT_EOF(fseek(fp, 0, SEEK_SET));
    ASSERT_SIZE(fwrite(info, sizeof(ClusterInfo), 1, fp), 1);

    // ���ɿ��д���
    size_t stack_offset = info->cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    FreeStack free_stack;
    memset(&free_stack, 0xFF, sizeof(FreeStack));
    free_stack.count = 0;
    free_stack.next_stack = EOC;

    for (cluster_t i = info->cluster_count - 1; i > CLUSTER_REV_MAX; i--)
    {
        free_stack.stack[free_stack.count++] = i;
        if (free_stack.count == MAX_STACK)
        {
            // ����ջд�����
            long offset = stack_offset + i * info->cluster_size; // ����Ŀ��ƫ����
            ASSERT_EOF(fseek(fp, offset, SEEK_SET));
            ASSERT_SIZE(fwrite(&free_stack, sizeof(FreeStack), 1, fp), 1);
            // ���ӿ���ջ
            free_stack.next_stack = i;
            free_stack.count = 0;
        }
    }
    // ���һ������ջд�� MMC
    ASSERT_EOF(fseek(fp, stack_offset, SEEK_SET));
    ASSERT_SIZE(fwrite(&free_stack, sizeof(FreeStack), 1, fp), 1);

    ASSERT_EOF(fclose(fp));
    return true;

faild:
    fclose(fp);
    return false;
}

bool ClusterMgr::LoadPartition(const char* file)
{
    opened = false;

    FILE* fp = fopen(file, "rb+");
    if (fp == NULL)
    {
        goto faild;
    }

    // ��ȡ meta �� info ��
    ASSERT_SIZE(fread(&info, sizeof(ClusterInfo), 1, fp), 1);

    fd = fp;

    // ��ʼ���ػ���
    size_t cache_size;
    cache_size = info.cluster_count / 3;
    if (cache_size > 1024)
    {
        cache_size = 1024;
    }
    inactive_cache = new LruCache<cluster_t, ClusterContainer*>(cache_size);
    active_cache = new SparseArray<cluster_t, ClusterContainer*>();

    MMC = _Fetch(CLUSTER_REV_MM);

    if (MMC == NULL)
    {
        goto faild;
    }

    opened = true;
    return true;

faild:
    if (fd != NULL)
    {
        fclose(fd);
        fd = NULL;
    }
    DELETE(MMC);
    DELETE(inactive_cache);
    DELETE(active_cache);
    return false;
}

bool ClusterMgr::ClosePartition()
{
    opened = false;
    Dispose(*MMC); // �ͷ�MMC��
    MMC = NULL;

    // ���ղ��ͷ����л���
    ClusterContainer* c;

    if ((c = inactive_cache->Next(true)) != NULL)
    {
        do
        {
            delete c;
        } while ((c = inactive_cache->Next(false)) != NULL);
    }

    if ((c = active_cache->Next(true)) != NULL)
    {
        do
        {
            delete c;
        } while ((c = active_cache->Next(false)) != NULL);
    }

    // ɾ������
    DELETE(inactive_cache);
    DELETE(active_cache);

    fclose(fd);
    fd = NULL;

    memset(&info, 0, sizeof(ClusterInfo));

    return true;
}

size_t ClusterMgr::GetClusterSize()
{
    return info.cluster_size;
}

cluster_t ClusterMgr::GetFreeCluster()
{
    return info.free_cluster_count;
}

ClusterContainer* ClusterMgr::_Fetch(cluster_t id)
{
    ClusterContainer* cluster;

    cluster = active_cache->Get(id);
    if (cluster != NULL)
    {
        cluster->NewRef();
        return cluster;
    }

    cluster = inactive_cache->Hit(id);
    if (cluster != NULL)
    {
        active_cache->Set(id, cluster);
        cluster->NewRef();
        return cluster;
    }

    // �����»����
    cluster = new ClusterContainer(this, id);
    if (cluster == NULL)
    {
        return NULL;
    }
    if (!cluster->Avaliable())
    {
        delete cluster;
        return NULL;
    }

    active_cache->Set(id, cluster);
    cluster->NewRef();
    return cluster;
}

cluster_t* ClusterMgr::Allocate(cluster_t count, cluster_t* out)
{
    cluster_t* allocation = out;
    cluster_t* p = allocation;
    cluster_t count_p = count;
    FreeStack free_stack;
    FreeStack allo_stack;
    ClusterContainer* next_cluster = NULL;

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    // ��ȡ MMC �� FreeStack
    ASSERT_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

    while (count_p > 0)
    {
        if (free_stack.count == 0) // ���MMC�п��д��þ����������һ������ջ
        {
            cluster_t next_stack = free_stack.next_stack;
            if (next_stack == EOC)
            { // �Բۣ��ռ�ľ���
                goto faild;
            }
            else
            {
                next_cluster = Fetch(next_stack);// ������һ����
                ASSERT_SIZE(next_cluster->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));// װ�����ջ
                Dispose(*next_cluster);
                next_cluster = NULL;

                // ���� MMC
                ASSERT_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
            }
        }
        cluster_t curr_free = free_stack.count; // ��ǰջ��ʣ�������
        cluster_t curr_alloc = curr_free >= count_p ? count_p : curr_free; // ��ǰһ��������Ĵ�����
        allo_stack.count = 0;

        for (; curr_alloc > 0; curr_alloc--)
        {
            curr_free--; // ����ջ����һ�����ôص�ջλ��
            allo_stack.stack[allo_stack.count++] = free_stack.stack[curr_free]; // ������Ŵӿ���ջ�ƶ�������ջ��
            free_stack.stack[curr_free] = EOC;
        }
        free_stack.count = curr_free;

        // ���� MMC
        ASSERT_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

        // �ӷ���ջ�ж������ݵ����������
        memcpy((void*)p, allo_stack.stack, allo_stack.count * sizeof(cluster_t));
        p += allo_stack.count;
        count_p -= allo_stack.count;

        // ���¿��д�
        info.free_cluster_count -= allo_stack.count;
    }

    // ����MMC
    ASSERT_SIZE(MMC->Write(0, 0, sizeof(ClusterInfo), (unsigned char*)&info), sizeof(ClusterInfo));

    return allocation;
faild:
    if (next_cluster != NULL)
    {
        Dispose(*next_cluster);
    }
    for (; allocation != p; --p) // ���շ�����෴˳���ͷ��ѷ����
    {
        Free(*p);
    }
    return NULL;
}

bool ClusterMgr::Free(cluster_t cluster)
{
    FreeStack free_stack;
    ClusterContainer* full_cluster = NULL;

    if (cluster <= CLUSTER_REV_MAX || cluster == EOC)
    {
        return false;
    }

    // ���鵱ǰ���Ƿ��ǻ״̬
    if (IsActive(cluster))
    {
        return false;
    }
    // ����ǻ�������Ƿ��иô�
    ClusterContainer* inactive = inactive_cache->Hit(cluster);
    DELETE(inactive); // ���ոô�

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    // ��ȡ MMC �� FreeStack
    ASSERT_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

    if (free_stack.count == MAX_STACK)
    {
        // �����ܳ��ֵ��������Ϊ����ռ�ʱ��MMCջΪ���һ���Ҫ�ٷ���ռ�ʱ�ŵ�����һ��ջ
        // ��ֻҪ�пռ䱻���䣬�� MMC �е�ջ��Ȼ����
        goto faild;
    }

    free_stack.stack[free_stack.count++] = cluster; // ���д���ջ

    if (free_stack.count == MAX_STACK)
    {
        // ջ����д�� cluster ָ��Ĵ���
        full_cluster = Fetch(cluster);
        ASSERT_SIZE(full_cluster->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
        Dispose(*full_cluster);
        full_cluster = NULL;

        // ���¿���ջ
        free_stack.count = 0;
        free_stack.next_stack = cluster;
    }

    info.free_cluster_count++;
    // ����MMC
    ASSERT_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
    ASSERT_SIZE(MMC->Write(0, 0, sizeof(ClusterInfo), (unsigned char*)&info), sizeof(ClusterInfo));

    return true;
faild:
    if (full_cluster != NULL)
    {
        Dispose(*full_cluster);
    }
    return false;
}

ClusterContainer* ClusterMgr::Fetch(cluster_t cluster)
{
    if (!opened)
    {
        return NULL;
    }

    return _Fetch(cluster);
}

bool ClusterMgr::Dispose(ClusterContainer& cluster)
{
    if (cluster.ReleaseRef())
    {
        active_cache->Remove(cluster.cluster);
        ClusterContainer* old = inactive_cache->Push(cluster.cluster, &cluster);
        DELETE(old);
    }

    return true;
}

bool ClusterMgr::Sync()
{
    ClusterContainer* c;
    bool ret = true;

    if ((c = inactive_cache->Next(true)) != NULL)
    {
        do
        {
            ret = c->Sync() && ret;
        } while ((c = inactive_cache->Next(false)) != NULL);
    }

    if ((c = active_cache->Next(true)) != NULL)
    {
        do
        {
            ret = c->Sync() && ret;
        } while ((c = active_cache->Next(false)) != NULL);
    }

    return ret;
}

bool ClusterMgr::IsActive(cluster_t id)
{
    ClusterContainer* cluster;

    cluster = active_cache->Get(id);

    return cluster != NULL;
}

ClusterContainer::ClusterContainer(ClusterMgr* mgr, cluster_t cluster) :
    cluster_mgr(mgr),
    fd(mgr->fd),
    info(&mgr->info),
    cluster(cluster),
    flag(0),
    ref(0)
{
    buffer = new unsigned __int8[info->cluster_size];

    // �Ӵ��̶����ڴ�
    long offset = cluster * info->cluster_size;
    ASSERT_EOF(fseek(fd, offset, SEEK_SET));

    ASSERT_SIZE(fread((void*)buffer, sizeof(unsigned __int8), info->cluster_size, fd), info->cluster_size);

    flag |= FLAG_AVAILABLE;

faild:
    return;
}

ClusterContainer::~ClusterContainer()
{
    Sync();
    if (buffer != NULL)
    {
        delete[] buffer;
    }
}

bool ClusterContainer::Sync()
{
    if (!Avaliable() || (flag & FLAG_DIRTY) == 0)
    {
        return true;
    }

    // ���ڴ�д�����
    long offset = cluster * info->cluster_size;
    ASSERT_EOF(fseek(fd, offset, SEEK_SET));

    ASSERT_SIZE(fwrite((void*)buffer, sizeof(unsigned __int8), info->cluster_size, fd), info->cluster_size);

    flag &= ~FLAG_DIRTY;
    flag &= ~MASK_DCOUNT;

    return true;

faild:
    return false;
}

void ClusterContainer::NewRef()
{
    ref++;
}

bool ClusterContainer::ReleaseRef()
{
    ref--;
    return ref == 0;
}

bool ClusterContainer::Modify()
{
    flag |= FLAG_DIRTY;
    unsigned __int16 dirty_count = flag & MASK_DCOUNT;
    if (dirty_count >= DIRTY_MAX)
    {
        return Sync();
    }

    dirty_count++;
    flag &= ~MASK_DCOUNT;
    flag |= (dirty_count & MASK_DCOUNT);

    return true;
}

bool ClusterContainer::Avaliable()
{
    return (flag & FLAG_AVAILABLE) == FLAG_AVAILABLE;
}

cluster_t ClusterContainer::GetCluster()
{
    return cluster;
}

size_t ClusterContainer::Read(size_t dst_offset, size_t src_offset, size_t count, unsigned __int8* dst)
{
    if (src_offset < 0)
    {
        return 0;
    }

    size_t size_max = info->cluster_size - src_offset;
    if (size_max < count)
    {
        count = size_max;
    }
    if (count <= 0)
    {
        return 0;
    }

    memcpy((void*)(dst + dst_offset), (void*)(buffer + src_offset), count);

    return count;
}

size_t ClusterContainer::Write(size_t dst_offset, size_t src_offset, size_t count, unsigned __int8* src)
{
    if (dst_offset < 0)
    {
        return 0;
    }

    size_t size_max = info->cluster_size - dst_offset;
    if (size_max < count)
    {
        count = size_max;
    }
    if (count <= 0)
    {
        return 0;
    }

    memcpy((void*)(buffer + dst_offset), (void*)(src + src_offset), count);

    Modify();

    return count;
}

size_t ClusterContainer::Memset(size_t dst_offset, size_t count, unsigned __int8 value)
{
    if (dst_offset < 0)
    {
        return 0;
    }

    size_t size_max = info->cluster_size - dst_offset;
    if (size_max < count)
    {
        count = size_max;
    }
    if (count <= 0)
    {
        return 0;
    }

    memset((void*)(buffer + dst_offset), value, count);

    Modify();
    return count;
}

