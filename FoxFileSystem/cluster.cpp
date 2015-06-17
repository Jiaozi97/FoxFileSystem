#include <cstring>

#include "cluster.h"

#define ASSET_EOF(v) do{if((v) == EOF)goto faild;}while(0)
#define ASSET_SIZE(v, size) do{if((v) != size)goto faild;}while(0)

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

    ASSET_EOF(fseek(fp, total_size - 1, SEEK_SET));
    ASSET_EOF(fputc(0, fp));

    info->free_cluster_count = info->cluster_size - 1;
    // д�� MMC
    ASSET_EOF(fseek(fp, 0, SEEK_SET));
    ASSET_SIZE(fwrite(info, sizeof(ClusterInfo), 1, fp), 1);

    // ���ɿ��д���
    size_t stack_offset = info->cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    FreeStack free_stack;
    free_stack.count = 0;
    free_stack.next_stack = 0;

    for (cluster_t i = info->cluster_count - 1; i > 0; i--)
    {
        free_stack.stack[free_stack.count++] = i;
        if (free_stack.count == MAX_STACK)
        {
            // ����ջд�����
            long offset = stack_offset + i * info->cluster_size; // ����Ŀ��ƫ����
            ASSET_EOF(fseek(fp, offset, SEEK_SET));
            ASSET_SIZE(fwrite(&free_stack, sizeof(FreeStack), 1, fp), 1);
            // ���ӿ���ջ
            free_stack.next_stack = i;
            free_stack.count = 0;
        }
    }
    // ���һ������ջд�� MMC
    ASSET_EOF(fseek(fp, stack_offset, SEEK_SET));
    ASSET_SIZE(fwrite(&free_stack, sizeof(FreeStack), 1, fp), 1);

    ASSET_EOF(fclose(fp));
    return true;

faild:
    fclose(fp);
    return false;
}

#define DELETE(a) do{if(a != NULL){delete a; a = NULL;}}while(0)

bool ClusterMgr::LoadPartition(const char* file)
{
    FILE* fp = fopen(file, "rb+");
    if (fp == NULL)
    {
        goto faild;
    }

    // ��ȡ meta �� info ��
    ASSET_SIZE(fread(&info, sizeof(ClusterInfo), 1, fp), 1);

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

    MMC = _Fetch(0);

    if (MMC == NULL)
    {
        goto faild;
    }

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
    cluster_t* allocation = new cluster_t[count];
    cluster_t* p = allocation;
    cluster_t count_p = count;
    FreeStack free_stack;
    FreeStack allo_stack;
    ClusterContainer* next_cluster = NULL;

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    // ��ȡ MMC �� FreeStack
    ASSET_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

    while (count_p > 0)
    {
        if (free_stack.count == 0) // ���MMC�п��д��þ����������һ������ջ
        {
            cluster_t next_stack = free_stack.next_stack;
            if (next_stack == 0)
            { // �Բۣ��ռ�ľ���
                goto faild;
            }
            else
            {
                next_cluster = Fetch(next_stack);// ������һ����
                ASSET_SIZE(next_cluster->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));// װ�����ջ
                Dispose(*next_cluster);
                next_cluster = NULL;

                // ���� MMC
                ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
            }
        }
        cluster_t curr_free = free_stack.count; // ��ǰջ��ʣ�������
        cluster_t curr_alloc = curr_free >= count_p ? count_p : curr_free; // ��ǰһ��������Ĵ�����
        allo_stack.count = 0;

        for (; curr_alloc > 0; curr_alloc--)
        {
            curr_free--; // ����ջ����һ�����ôص�ջλ��
            allo_stack.stack[allo_stack.count++] = free_stack.stack[curr_free]; // ������Ŵӿ���ջ�ƶ�������ջ��
        }
        free_stack.count = curr_free;

        // ���� MMC
        ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

        // �ӷ���ջ�ж������ݵ����������
        memcpy((void*)p, allo_stack.stack, allo_stack.count * sizeof(cluster_t));
        p += allo_stack.count;

        // ���¿��д�
        info.free_cluster_count -= allo_stack.count;
    }

    // ����MMC
    ASSET_SIZE(MMC->Write(0, 0, sizeof(ClusterInfo), (unsigned char*)&info), sizeof(ClusterInfo));

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
    delete[] allocation;
    return NULL;
}

bool ClusterMgr::Free(cluster_t cluster)
{
    FreeStack free_stack;
    ClusterContainer* full_cluster = NULL;

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // ����ջ�ڴ��ϵ�ƫ����

    // ��ȡ MMC �� FreeStack
    ASSET_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

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
        ASSET_SIZE(full_cluster->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
        Dispose(*full_cluster);
        full_cluster = NULL;

        // ����MMC�Ŀ���ջ
        free_stack.count = 0;
        free_stack.next_stack = cluster;
        ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
    }

    info.free_cluster_count++;
    // ����MMC
    ASSET_SIZE(MMC->Write(0, 0, sizeof(ClusterInfo), (unsigned char*)&info), sizeof(ClusterInfo));

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
    if (fseek(fd, offset, SEEK_SET) == EOF)
    {
        return;
    }

    size_t count = fread((void*)buffer, sizeof(unsigned __int8), info->cluster_size, fd);
    if (count != info->cluster_size)
    {
        return;
    }

    flag |= FLAG_AVAILABLE;
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
    if (fseek(fd, offset, SEEK_SET) == EOF)
    {
        return false;
    }

    size_t count = fwrite((void*)buffer, sizeof(unsigned __int8), info->cluster_size, fd);
    if (count != info->cluster_size)
    {
        return false;
    }

    flag &= ~FLAG_DIRTY;
    flag &= ~MASK_DCOUNT;

    return true;
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

bool ClusterContainer::Avaliable()
{
    return (flag & FLAG_AVAILABLE) == FLAG_AVAILABLE;
}

size_t ClusterContainer::Read(size_t dst_offset, size_t src_offset, size_t count, unsigned char* dst)
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

size_t ClusterContainer::Write(size_t dst_offset, size_t src_offset, size_t count, unsigned char* src)
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

    flag |= FLAG_DIRTY;

    memcpy((void*)(buffer + dst_offset), (void*)(src + src_offset), count);

    unsigned __int16 dirty_count = flag & MASK_DCOUNT;
    if (dirty_count >= DIRTY_MAX)
    {
        Sync();
    }
    else
    {
        dirty_count++;
        flag &= ~MASK_DCOUNT;
        flag |= (dirty_count & MASK_DCOUNT);
    }

    return count;
}
