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
    // 写入 MMC
    ASSET_EOF(fseek(fp, 0, SEEK_SET));
    ASSET_SIZE(fwrite(info, sizeof(ClusterInfo), 1, fp), 1);

    // 构成空闲簇链
    size_t stack_offset = info->cluster_size - sizeof(FreeStack); // 空闲栈在簇上的偏移量

    FreeStack free_stack;
    free_stack.count = 0;
    free_stack.next_stack = 0;

    for (cluster_t i = info->cluster_count - 1; i > 0; i--)
    {
        free_stack.stack[free_stack.count++] = i;
        if (free_stack.count == MAX_STACK)
        {
            // 空闲栈写入簇中
            long offset = stack_offset + i * info->cluster_size; // 计算目标偏移量
            ASSET_EOF(fseek(fp, offset, SEEK_SET));
            ASSET_SIZE(fwrite(&free_stack, sizeof(FreeStack), 1, fp), 1);
            // 链接空闲栈
            free_stack.next_stack = i;
            free_stack.count = 0;
        }
    }
    // 最后一个空闲栈写入 MMC
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

    // 读取 meta 到 info 中
    ASSET_SIZE(fread(&info, sizeof(ClusterInfo), 1, fp), 1);

    fd = fp;

    // 初始化簇缓存
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
    Dispose(*MMC); // 释放MMC块
    MMC = NULL;

    // 回收并释放所有缓存
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

    // 删除缓存
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

    // 创建新缓存块
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

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // 空闲栈在簇上的偏移量

    // 读取 MMC 的 FreeStack
    ASSET_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

    while (count_p > 0)
    {
        if (free_stack.count == 0) // 如果MMC中空闲簇用尽，则调入下一个空闲栈
        {
            cluster_t next_stack = free_stack.next_stack;
            if (next_stack == 0)
            { // 卧槽！空间耗尽！
                goto faild;
            }
            else
            {
                next_cluster = Fetch(next_stack);// 调入下一个簇
                ASSET_SIZE(next_cluster->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));// 装入空闲栈
                Dispose(*next_cluster);
                next_cluster = NULL;

                // 更新 MMC
                ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
            }
        }
        cluster_t curr_free = free_stack.count; // 当前栈中剩余簇数量
        cluster_t curr_alloc = curr_free >= count_p ? count_p : curr_free; // 当前一批最多分配的簇数量
        allo_stack.count = 0;

        for (; curr_alloc > 0; curr_alloc--)
        {
            curr_free--; // 空闲栈中下一个可用簇的栈位置
            allo_stack.stack[allo_stack.count++] = free_stack.stack[curr_free]; // 将簇序号从空闲栈移动到分配栈中
        }
        free_stack.count = curr_free;

        // 更新 MMC
        ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

        // 从分配栈中读出数据到输出数组中
        memcpy((void*)p, allo_stack.stack, allo_stack.count * sizeof(cluster_t));
        p += allo_stack.count;

        // 更新空闲簇
        info.free_cluster_count -= allo_stack.count;
    }

    // 更新MMC
    ASSET_SIZE(MMC->Write(0, 0, sizeof(ClusterInfo), (unsigned char*)&info), sizeof(ClusterInfo));

    return allocation;
faild:
    if (next_cluster != NULL)
    {
        Dispose(*next_cluster);
    }
    for (; allocation != p; --p) // 按照分配的相反顺序释放已分配簇
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

    size_t stack_offset = info.cluster_size - sizeof(FreeStack); // 空闲栈在簇上的偏移量

    // 读取 MMC 的 FreeStack
    ASSET_SIZE(MMC->Read(0, stack_offset, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));

    if (free_stack.count == MAX_STACK)
    {
        // 不可能出现的情况，因为分配空间时当MMC栈为空且还需要再分配空间时才调入下一个栈
        // 故只要有空间被分配，则 MMC 中的栈必然不满
        goto faild;
    }

    free_stack.stack[free_stack.count++] = cluster; // 空闲簇入栈

    if (free_stack.count == MAX_STACK)
    {
        // 栈满，写入 cluster 指向的簇中
        full_cluster = Fetch(cluster);
        ASSET_SIZE(full_cluster->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
        Dispose(*full_cluster);
        full_cluster = NULL;

        // 更新MMC的空闲栈
        free_stack.count = 0;
        free_stack.next_stack = cluster;
        ASSET_SIZE(MMC->Write(stack_offset, 0, sizeof(FreeStack), (unsigned char*)&free_stack), sizeof(FreeStack));
    }

    info.free_cluster_count++;
    // 更新MMC
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

    // 从磁盘读入内存
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

    // 从内存写入磁盘
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
