#define _USE_32BIT_TIME_T

#include <ctime>

#include "node.h"
#include "util.h"

#define SEEK_EMPTY -2
#define SEEK_FAIL -3

Node::Node(NodeMgr* mgr, ClusterContainer* mc) :
    node_mgr(mgr),
    MC(mc),
    cdo(0),
    flag(0),
    index_boundary(mgr->index_boundary)
{
    memset(&inode, 0, sizeof(INode));

    LoadCluster(0);
}

bool Node::Init()
{
    inode.time_create = inode.time_modify = inode.time_visit = time(NULL);
    int i;
    for (i = 0; i < MAX_DIRECT; i++)
    {
        inode.index_direct[i] = EOC;
    }
    for (i = 0; i < MAX_INDIRECT; i++)
    {
        inode.index_indir[i] = EOC;
    }

    ASSERT_SIZE(MC->Write(0, 0, sizeof(INode), (unsigned __int8*)&inode), sizeof(INode));
    ASSERT_FALSE(Modify());

    return true;
faild:
    return false;
}

bool Node::Load()
{
    ASSERT_SIZE(MC->Read(0, 0, sizeof(INode), (unsigned __int8*)&inode), sizeof(INode));
    inode.time_visit = time(NULL);
    Modify();

    return true;
faild:
    return false;
}

bool Node::Modify()
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

__int8 Node::ClusterSeek(size_t offset)
{
    if (offset == -1) // ����������ڼ���ߴ�߽��ʱ����
    {
        seek_cluster_offset = sizeof(INode) + offset;
        return -1; // ����Ҫ������ֱ�ӿ��Դ����� MC ��
    }

    if (offset < 0)
    {
        return SEEK_FAIL; // ����
    }

    if (offset < index_boundary.no_index)
    {
        seek_cluster_offset = sizeof(INode) + offset;
        return -1; // ����Ҫ������ֱ�ӿ��Դ����� MC ��
    }
    int i;
    file_size_t size_prev = index_boundary.no_index;
    file_size_t size_curr, size_level_max;
    size_level_max = index_boundary.index_direct[MAX_DIRECT - 1];
    if (offset < size_level_max) {
        for (i = 0; i < MAX_DIRECT; i++) // ֱ������
        {
            size_curr = index_boundary.index_direct[i];
            if (offset < size_curr)
            {
                seek_cluster_offset = (size_t)(offset - size_prev);
                seek_index[0] = i;
                return 0;
            }
            size_prev = size_curr;
        }
    }
    else
    {
        size_prev = size_level_max;
    }

    int j;
    size_level_max = index_boundary.index_indir[MAX_INDIRECT - 1];
    if (offset < size_level_max) {
        for (i = 0; i < MAX_INDIRECT; i++) // �������
        {
            size_curr = index_boundary.index_indir[i];
            if (offset < size_curr)
            {
                seek_cluster_offset = (size_t)(offset - size_prev);
                for (j = 0; j <= i; j++)
                {
                    seek_index[j + 1] = (size_t)(seek_cluster_offset / index_boundary.size_level[i - j]);
                    seek_cluster_offset %= index_boundary.size_level[i - j];
                }
                seek_index[0] = i;
                return i + 1;
            }
            size_prev = size_curr;
        }
    }
    else
    {
        size_prev = size_level_max;
    }

    return SEEK_FAIL;
}

bool Node::_LoadCluster(size_t offset)
{
    ClusterContainer* t_cluster = NULL;

    __int8 index_depth = ClusterSeek(offset);
    if (index_depth == SEEK_FAIL)
    {
        return false;
    }

    t_cluster = LoadClusterByIndex(index_depth, seek_index);

    if (t_cluster == NULL)
    {
        return false;
    }

    target_cluster = t_cluster;
    target_offset = offset;
    target_cluster_offset = seek_cluster_offset;
    return true;
}

ClusterContainer* Node::LoadClusterByIndex(__int8 depth, size_t* index)
{
    ClusterContainer* index_cluster = NULL;
    if (depth == -1)
    {
        ClusterContainer* next = node_mgr->cluster_service->Fetch(MC->GetCluster());
        ASSERT_NULL(next);

        return next;
    }
    if (depth == 0) // ֱ������
    {
        ClusterContainer* next = node_mgr->cluster_service->Fetch(inode.index_direct[index[0]]);
        ASSERT_NULL(next);

        return next;
    }

    // �������
    cluster_t start_index;
    switch (depth)
    {
    case 1:
    case 2:
    case 3:
        start_index = inode.index_indir[depth - 1];
        break;
    default:
        goto faild;
    }

    index_cluster = node_mgr->cluster_service->Fetch(start_index);
    ASSERT_NULL(index_cluster);

    int i;
    cluster_t next_index;
    for (i = 1; i <= depth; i++)
    {
        ASSERT_SIZE(index_cluster->Read(0, index[i] * sizeof(cluster_t), sizeof(cluster_t), (unsigned __int8*)&next_index), sizeof(cluster_t)); // �������ڵ������һ��Ĵغ�
        node_mgr->cluster_service->Dispose(*index_cluster); //�ͷ������ڵ�
        index_cluster = node_mgr->cluster_service->Fetch(next_index); // ��ȡ��һ���
        ASSERT_NULL(index_cluster);
    }

    return index_cluster;

faild:
    if (index_cluster != NULL)
    {
        node_mgr->cluster_service->Dispose(*index_cluster);
    }
    return NULL;
}

bool Node::LoadCluster(size_t offset)
{
    ASSERT_FALSE(_LoadCluster(offset)); // ����Ŀ���

    // �ͷŵ�ǰ�أ�����дָ���´�
    ClusterContainer* old = current;
    if (old != NULL) {
        old->Sync();
        node_mgr->cluster_service->Dispose(*old); //�ͷžɴ�
    }
    current = target_cluster;
    cdo = target_offset;
    cco = target_cluster_offset;

    return true;
faild:
    return false;
}

#define DISPOSE_CLUSTER(_c) do{ClusterContainer* tmp_index = _c; _c = NULL;ASSERT_FALSE(node_mgr->cluster_service->Dispose(*tmp_index));}while(0)

#define SEEK_SIZE(s) \
do{                                                           \
    if ((seek_depth_##s = ClusterSeek(s - 1)) == SEEK_FAIL)       \
    {                                                         \
        goto faild;                                           \
    }                                                         \
    seek_cluster_offset_##s = seek_cluster_offset;            \
    memcpy(seek_index_##s, seek_index, sizeof(seek_index));   \
} while(0)

bool Node::Shrink(size_t curr, size_t target)
{
    size_t seek_index_curr[MAX_INDIRECT + 1];
    size_t seek_cluster_offset_curr; // ����ƫ��������ʱ�ò���������չʱ��Ҫ����������չ���Ŀռ���0�����
    __int8 seek_depth_curr;
    size_t seek_index_target[MAX_INDIRECT + 1];
    size_t seek_cluster_offset_target;
    __int8 seek_depth_target;

    SEEK_SIZE(curr);
    SEEK_SIZE(target);

    if (seek_depth_curr != seek_depth_target) // ���ڲ�ͬ�㼶�������Ƴ����߲㼶��ȫ�����
    {
        // ��Ϊ���depthΪ-1����curr��targer����ȣ��Ҵ�ʱ����С����seek_depth_curr��Ȼ>=0
        // �ȴ���������
        if (seek_depth_curr > 0)
        {
            __int8 fin_depth;
            int i = 1;
            if (seek_depth_target > 0)
            {
                i = seek_depth_target + 1;
            }
            fin_depth = i - 1;
            for (; i < seek_depth_curr; i++)
            {
                ASSERT_FALSE(RemoveClusterRight(i, inode.index_indir[i - 1], NULL));
                inode.index_indir[i - 1] = EOC;
            }
            ASSERT_FALSE(RemoveClusterRight(i, inode.index_indir[i - 1], &seek_index_curr[1]));
            inode.index_indir[i - 1] = EOC;
            // ������ǰ����
            seek_depth_curr = fin_depth;
            //seek_cluster_offset_curr = index_boundary.size_level[0] - 1;
            if (seek_depth_curr == 0)
            {
                seek_index_curr[0] = MAX_DIRECT - 1;
            }
            else
            {
                seek_index_curr[0] = seek_depth_curr - 1;
                size_t s2 = index_boundary.index_per_cluster - 1;
                for (i = 1; i <= seek_depth_curr; i++)
                {
                    seek_index_curr[i] = s2;
                }
            }
        }
        // ����ֱ������
        if (seek_depth_target == -1) // ��ʱ���� seek_depth_curr == 0, seek_depth_target == -1
        {
            size_t i;
            for (i = 0; i <= seek_index_curr[0]; i++)
            {
                ASSERT_FALSE(RemoveClusterRight(0, inode.index_direct[i], NULL));
                inode.index_direct[i] = EOC;
            }
            // ������ǰ����
            seek_depth_curr = -1;
            //seek_cluster_offset_curr = index_boundary.size_level[0] - 1;
        }
    }

    // ������ͬһ�㼶������
    if (seek_depth_curr > 0)
    {// �������
        // �ж��Ƿ���ͬһ������
        int i;
        bool same_cluster = true;
        for (i = 1; i <= seek_depth_curr; i++)
        {
            if (seek_index_curr[i] != seek_index_target[i])
            {
                same_cluster = false;
                break;
            }
        }
        if (!same_cluster) // ����ͬһ������
        {
            // ɾ�������
            ASSERT_FALSE(RemoveCluster(seek_depth_curr, inode.index_indir[seek_depth_curr - 1], &seek_index_target[1], &seek_index_curr[1]));
            //��������
            memcpy(seek_index_curr, seek_index_target, sizeof(seek_index_target));
            //seek_cluster_offset_curr = index_boundary.size_level[0] - 1;
        }
    }
    else if (seek_depth_curr == 0)
    { // ֱ������
        size_t i;
        for (i = seek_index_target[0] + 1; i <= seek_index_curr[0]; i++)
        {
            ASSERT_FALSE(RemoveClusterRight(0, inode.index_direct[i], NULL));
            inode.index_direct[i] = EOC;
        }
        // ������ǰ����
        //seek_index_target[0] = seek_index_curr[0];
        //seek_cluster_offset_curr = index_boundary.size_level[0] - 1;
    }

    // �������ͬһ�����е����
    // �����ò�Ʋ�����Ҫ����
    // ֱ��ָ���һ�¾ͺ���

    // Ȼ�����Ǹ����ļ�������
    inode.size = target;
    ASSERT_FALSE(Modify());

    return true;
faild:
    return false;
}

bool Node::Expand(size_t curr, size_t target)
{
    size_t seek_index_curr[MAX_INDIRECT + 1];
    size_t seek_cluster_offset_curr;
    __int8 seek_depth_curr;
    size_t seek_index_target[MAX_INDIRECT + 1];
    size_t seek_cluster_offset_target;
    __int8 seek_depth_target;

    cluster_t next_cluster;
    ClusterContainer* current_cluster = NULL;

    SEEK_SIZE(curr);
    SEEK_SIZE(target);

    if (seek_depth_curr != seek_depth_target)
    {
        // �ȴ���������
        if (seek_depth_target > 0)
        {
            __int8 fin_depth;
            int i = 1;
            if (seek_depth_curr > 0)
            {
                i = seek_depth_curr + 1;
            }
            fin_depth = i - 1;
            for (; i < seek_depth_target; i++)
            {
                ASSERT_EOC(next_cluster = BuildClusterRight(i, NULL));
                inode.index_indir[i - 1] = next_cluster;
            }
            ASSERT_EOC(next_cluster = BuildClusterRight(i, &seek_index_target[1]));
            inode.index_indir[i - 1] = next_cluster;
            // ������ǰ����
            seek_depth_target = fin_depth;
            seek_cluster_offset_target = (cluster_size_t)index_boundary.size_level[0] - 1;
            if (seek_depth_target == 0)
            {
                seek_index_target[0] = MAX_DIRECT - 1;
            }
            else
            {
                seek_index_target[0] = seek_depth_target - 1;
                size_t s2 = index_boundary.index_per_cluster - 1;
                for (i = 1; i <= seek_depth_target; i++)
                {
                    seek_index_target[i] = s2;
                }
            }
        }
        // ����ֱ������
        if (seek_depth_curr == -1) // ��ʱ���� seek_depth_curr == -1, seek_depth_target == 0
        {
            size_t i;
            for (i = 0; i <= seek_index_target[0]; i++)
            {
                ASSERT_EOC(next_cluster = BuildClusterRight(0, NULL));
                inode.index_direct[i] = next_cluster;
            }
            // ������ǰ����
            seek_depth_target = -1;
            seek_cluster_offset_target = (cluster_size_t)index_boundary.size_level[0] - 1;
        }
    }

    // ������ͬһ�㼶������
    if (seek_depth_target > 0)
    {// �������
        // �ж��Ƿ���ͬһ������
        int i;
        bool same_cluster = true;
        for (i = 1; i <= seek_depth_curr; i++)
        {
            if (seek_index_curr[i] != seek_index_target[i])
            {
                same_cluster = false;
                break;
            }
        }
        if (!same_cluster) // ����ͬһ������
        {
            ASSERT_FALSE(BuildCluster(seek_depth_target, inode.index_indir[seek_depth_target - 1], &seek_index_curr[1], &seek_index_target[1]));
            //��������
            memcpy(seek_index_target, seek_index_curr, sizeof(seek_index_curr));
            seek_cluster_offset_target = (cluster_size_t)index_boundary.size_level[0] - 1;
        }
    }
    else if (seek_depth_target == 0)
    {
        size_t i;
        for (i = seek_index_curr[0] + 1; i <= seek_index_target[0]; i++)
        {
            ASSERT_EOC(next_cluster = BuildClusterRight(0, NULL));
            inode.index_direct[i] = next_cluster;
        }
        // ������ǰ����
        seek_index_target[0] = seek_index_curr[0];
        seek_cluster_offset_target = (cluster_size_t)index_boundary.size_level[0] - 1;
    }

    // �������ͬһ�����е����
    // ��������Ҫ����������ֽ���0���
    ASSERT_NULL(current_cluster = LoadClusterByIndex(seek_depth_target, seek_index_target));
    // ��0����������
    size_t new_size = seek_cluster_offset_target - seek_cluster_offset_curr;
    ASSERT_SIZE(current_cluster->Memset(seek_cluster_offset_curr + 1, new_size, 0), new_size);
    DISPOSE_CLUSTER(current_cluster);

    // Ȼ�����Ǹ����ļ�������
    inode.size = target;

    ASSERT_FALSE(Modify());

    return true;
faild:
    if (current_cluster != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_cluster);
    }
    return false;
}

#undef SEEK_SIZE

#define READ_INDEX(_i) ASSERT_SIZE(current_index->Read(0, (_i) * sizeof(cluster_t), sizeof(cluster_t), (unsigned __int8*)&next_index), sizeof(cluster_t))
#define WRITE_INDEX(_i,_index) ASSERT_SIZE(current_index->Write((_i) * sizeof(cluster_t), 0, sizeof(cluster_t), (unsigned __int8*)&_index), sizeof(cluster_t))

bool Node::RemoveClusterRight(__int8 depth, cluster_t index, size_t* edge_right)
{
    ClusterContainer* current_index = NULL;
    cluster_t next_index;
    size_t i;
    if (depth > 0)
    {
        depth--;
        ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(index)); // �򿪱�������

        if (edge_right == NULL) // ���ڱ�Ե�ڵ���
        {
            for (i = 0; i < index_boundary.index_per_cluster; i++)
            {
                READ_INDEX(i); // ��ȡ��i���ڵ�����
                ASSERT_FALSE(RemoveClusterRight(depth, next_index, NULL));
            }
        }
        else
        {
            size_t edge_this = *edge_right;
            for (i = 0; i < edge_this; i++)
            {
                READ_INDEX(i); // ��ȡ��Ե֮ǰ�Ľڵ�
                ASSERT_FALSE(RemoveClusterRight(depth, next_index, NULL));
            }
            READ_INDEX(edge_this); // ��ȡ��Ե�ڵ�����
            ASSERT_FALSE(RemoveClusterRight(depth, next_index, &edge_right[1]));
        }

        DISPOSE_CLUSTER(current_index);// �ر�������
    }

    ASSERT_FALSE(node_mgr->cluster_service->Free(index)); // �ͷ�Ŀ���

    return true;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    return false;
}

bool Node::RemoveClusterLeft(__int8 depth, cluster_t index, size_t* edge_left)
{
    ClusterContainer* current_index = NULL;
    cluster_t next_index;
    size_t i;
    if (depth == 0)
    {
        return true;
    }
    depth--;

    ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(index)); // �򿪱�������

    for (i = *edge_left + 1; i < index_boundary.index_per_cluster; i++)
    {
        READ_INDEX(i); // ��ȡ��i���ڵ�����
        ASSERT_FALSE(RemoveClusterRight(depth, next_index, NULL));
    }

    READ_INDEX(*edge_left); // ��ȡ���Ե�ڵ�

    DISPOSE_CLUSTER(current_index); // �ر�������

    ASSERT_FALSE(RemoveClusterLeft(depth, next_index, &edge_left[1])); // ���������

    return true;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    return false;
}

bool Node::RemoveCluster(__int8 depth, cluster_t index, size_t* edge_left, size_t* edge_right)
{
    ClusterContainer* current_index = NULL;
    cluster_t next_index;
    if (depth == 0)
    {
        goto succ;
    }
    depth--;

    ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(index)); // �򿪱�������

    if (*edge_left == *edge_right)
    {
        READ_INDEX(*edge_left);
        DISPOSE_CLUSTER(current_index); // �ر�������

        ASSERT_FALSE(RemoveCluster(depth, next_index, &edge_left[1], &edge_right[1]));

        goto succ;
    }

    // ɾ���Ҳ������֧
    size_t i;
    size_t edge_this = *edge_right;
    for (i = *edge_left + 1; i < edge_this; i++)
    {
        READ_INDEX(i); // ��ȡ��Ե֮ǰ�Ľڵ�
        ASSERT_FALSE(RemoveClusterRight(depth, next_index, NULL));
    }
    READ_INDEX(edge_this); // ��ȡ��Ե�ڵ�����
    ASSERT_FALSE(RemoveClusterRight(depth, next_index, &edge_right[1]));

    READ_INDEX(*edge_left); // ��ȡ���Ե�ڵ�

    DISPOSE_CLUSTER(current_index); // �ر�������

    ASSERT_FALSE(RemoveClusterLeft(depth, next_index, &edge_left[1])); // ���������

succ:
    return true;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    return false;
}

cluster_t Node::BuildClusterRight(__int8 depth, size_t* edge_right)
{
    ClusterContainer* current_index = NULL;
    cluster_t current_cluster = EOC;
    cluster_t next_index;
    size_t i;

    ASSERT_NULL(node_mgr->cluster_service->Allocate(1, &current_cluster));
    ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(current_cluster));

    if (depth > 0)
    {
        depth--;

        if (edge_right == NULL)
        {
            for (i = 0; i < index_boundary.index_per_cluster; i++)
            {
                ASSERT_EOC(next_index = BuildClusterRight(depth, NULL)); // ����ײ�����
                WRITE_INDEX(i, next_index);
            }
        }
        else
        {
            size_t edge_this = *edge_right;
            for (i = 0; i < edge_this; i++)
            {
                ASSERT_EOC(next_index = BuildClusterRight(depth, NULL)); // ����ײ�����
                WRITE_INDEX(i, next_index);
            }
            ASSERT_EOC(next_index = BuildClusterRight(depth, &edge_right[1])); // ����ײ�����
            WRITE_INDEX(edge_this, next_index);
        }
    }
    else
    {
        ASSERT_FALSE(current_index->Memset(0, (cluster_size_t)index_boundary.size_level[0], 0)); // �������
    }

    DISPOSE_CLUSTER(current_index);

    return current_cluster;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    if (current_cluster != EOC)
    {
        node_mgr->cluster_service->Free(current_cluster);
    }
    return EOC;
}

bool Node::BuildClusterLeft(__int8 depth, cluster_t index, size_t* edge_left)
{
    ClusterContainer* current_index = NULL;
    cluster_t next_index;
    size_t i;
    if (depth == 0)
    {
        return true;
    }
    depth--;

    ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(index)); // �򿪱�������

    for (i = *edge_left + 1; i < index_boundary.index_per_cluster; i++)
    {
        ASSERT_EOC(next_index = BuildClusterRight(depth, NULL)); // ����ײ�����
        WRITE_INDEX(i, next_index);
    }

    READ_INDEX(*edge_left); // ��ȡ���Ե�ڵ�

    DISPOSE_CLUSTER(current_index); // �ر�������

    ASSERT_FALSE(BuildClusterLeft(depth, next_index, &edge_left[1])); // ���������

    return true;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    return false;
}

bool Node::BuildCluster(__int8 depth, cluster_t index, size_t* edge_left, size_t* edge_right)
{
    ClusterContainer* current_index = NULL;
    cluster_t next_index;
    if (depth == 0)
    {
        goto succ;
    }
    depth--;

    ASSERT_NULL(current_index = node_mgr->cluster_service->Fetch(index)); // �򿪱�������

    if (*edge_left == *edge_right)
    {
        READ_INDEX(*edge_left);
        DISPOSE_CLUSTER(current_index); // �ر�������

        ASSERT_FALSE(BuildCluster(depth, next_index, &edge_left[1], &edge_right[1]));

        goto succ;
    }

    // �ȴ����Ҳ��֧
    size_t i;
    size_t edge_this = *edge_right;
    for (i = *edge_left + 1; i < edge_this; i++)
    {
        ASSERT_EOC(next_index = BuildClusterRight(depth, NULL)); // ����ײ�����
        WRITE_INDEX(i, next_index);
    }
    ASSERT_EOC(next_index = BuildClusterRight(depth, &edge_right[1])); // �����Ե�ڵ�
    WRITE_INDEX(i, next_index);

    READ_INDEX(*edge_left); // ��ȡ���Ե�ڵ�

    DISPOSE_CLUSTER(current_index); // �ر�������

    ASSERT_FALSE(BuildClusterLeft(depth, next_index, &edge_left[1])); // ���������
succ:
    return true;
faild:
    if (current_index != NULL)
    {
        node_mgr->cluster_service->Dispose(*current_index);
    }
    return false;
}

cluster_t Node::GetNodeId()
{
    return MC->GetCluster();
}

bool Node::Sync()
{
    if ((flag & FLAG_DIRTY) == 0)
    {
        return true;
    }

    inode.time_modify = time(NULL);
    // MC����д��
    ASSERT_SIZE(MC->Write(0, 0, sizeof(INode), (unsigned __int8*)&inode), sizeof(INode));

    if (current != MC)
    {
        ASSERT_FALSE(current->Sync());
    }
    ASSERT_FALSE(MC->Sync());

    flag &= ~FLAG_DIRTY;
    flag &= ~MASK_DCOUNT;

    return true;
faild:
    return false;
}

unsigned __int16 Node::GetMode(unsigned __int16 mask)
{
    return inode.mode & mask;
}

bool Node::SetMode(unsigned __int16 mode, unsigned __int16 mask)
{
    mode &= mask;
    if (GetMode(mask) == mode)
    {
        return true;
    }

    // TODO: ����Ȩ�޺���Ч����֤

    inode.mode = inode.mode & ~mask | mode;

    ASSERT_FALSE(Modify());

    return true;
faild:
    return false;
}

size_t Node::GetSize()
{
    return inode.size;
}

bool Node::Truncate(size_t size)
{
    size_t s = GetSize();
    if (size == s)
    {
        return true;
    }

    return size > s ? Expand(s, size) : Shrink(s, size);
}

size_t Node::GetPointer()
{
    return cdo;
}

__int64 Node::Seek(size_t offset)
{
    if (offset == cdo)
    {
        return cdo;
    }
    if (offset < 0 || offset >= GetSize())
    {
        goto faild;
    }

    ASSERT_FALSE(LoadCluster((size_t)offset));

    return cdo;
faild:
    return EOF;
}

size_t Node::Write(void const* buffer, size_t size)
{
    // �ȼ���ʣ��ռ乻������ȡ
    size_t max_size = GetSize() - cdo;
    if (max_size < size)
    {
        size = max_size;
    }

    if (size == 0)
    {
        return 0;
    }

    size_t offset = 0;
    size_t cluster_size = node_mgr->cluster_service->GetClusterSize();
    size_t available_in_cluster; // ��ǰ�ؿ�������

    while (size > 0)
    {
        available_in_cluster = cluster_size - cco;
        if (available_in_cluster > size)
        {
            available_in_cluster = size;
        }
        if (available_in_cluster == 0) // ��ǰ��û�пռ����д��
        {
            // ������һ��
            ASSERT_FALSE(LoadCluster(cdo));
            continue;
        }
        ASSERT_SIZE(current->Write(cco, offset, available_in_cluster, (unsigned char*)buffer), available_in_cluster);

        cco += available_in_cluster;
        cdo += available_in_cluster;
        offset += available_in_cluster;
        size -= available_in_cluster;
    }
    Modify();

faild:
    return offset;
}

size_t Node::Read(void* buffer, size_t size)
{
    // �ȼ���ʣ�����ݹ�������ȡ
    size_t max_size = GetSize() - cdo;
    if (max_size < size)
    {
        size = max_size;
    }

    if (size == 0)
    {
        return 0;
    }

    size_t offset = 0;
    size_t cluster_size = node_mgr->cluster_service->GetClusterSize();
    size_t available_in_cluster; // ��ǰ�ؿ�������
    while (size > 0)
    {
        available_in_cluster = cluster_size - cco;
        if (available_in_cluster > size)
        {
            available_in_cluster = size;
        }
        if (available_in_cluster == 0) // ��ǰ��û�����ݿ��Զ�ȡ
        {
            // ������һ��
            ASSERT_FALSE(LoadCluster(cdo));
            continue;
        }
        ASSERT_SIZE(current->Read(offset, cco, available_in_cluster, (unsigned char*)buffer), available_in_cluster);

        cco += available_in_cluster;
        cdo += available_in_cluster;
        offset += available_in_cluster;
        size -= available_in_cluster;
    }

faild:
    return offset;
}

NodeMgr::NodeMgr(ClusterMgr* cluster_mgr) :
    cluster_service(cluster_mgr)
{
    size_t cluster_size = cluster_mgr->GetClusterSize();
    size_t index_per_cluster = cluster_size / sizeof(cluster_t);

    index_boundary.index_per_cluster = index_per_cluster;
    index_boundary.size_level[0] = cluster_size;

    // ����ÿһ������֧�ֵ�������ݳ���
    file_size_t s, s2;
    index_boundary.no_index = s = cluster_size - sizeof(INode);
    int i;
    for (i = 0; i < MAX_DIRECT; i++)
    {
        s += cluster_size;
        index_boundary.index_direct[i] = s;
    }

    s2 = cluster_size * index_per_cluster;
    s += s2;
    for (i = 0; i < MAX_INDIRECT; i++)
    {
        index_boundary.index_indir[i] = s;
        index_boundary.size_level[i + 1] = s2;
        s2 *= index_per_cluster;
        s += s2;
    }
}

Node* NodeMgr::CreateRootNode()
{
    ClusterContainer* cluster = cluster_service->Fetch(CLUSTER_REV_SECONDARY);
    ASSERT_NULL(cluster);

    Node* node = CreateNode(cluster);
    ASSERT_NULL(node);

    return node;
faild:
    if (cluster != NULL)
    {
        cluster_service->Dispose(*cluster);
    }
    return NULL;
}

Node* NodeMgr::OpenRootNode()
{
    return OpenNode(CLUSTER_REV_SECONDARY);
}

Node* NodeMgr::CreateNode(ClusterContainer* mc)
{
    Node* node = new Node(this, mc);
    ASSERT_NULL(node);

    ASSERT_FALSE(node->Init());

    return node;
faild:
    DELETE(node);
    return NULL;
}

Node* NodeMgr::CreateNode()
{
    cluster_t node_cluster[1];
    node_cluster[0] = EOC;

    ASSERT_NULL(cluster_service->Allocate(1, node_cluster));

    ClusterContainer* cluster = cluster_service->Fetch(node_cluster[0]);
    ASSERT_NULL(cluster);

    Node* node = CreateNode(cluster);
    ASSERT_NULL(node);

    return node;
faild:
    if (cluster != NULL)
    {
        cluster_service->Dispose(*cluster);
    }
    cluster_service->Free(node_cluster[0]);
    return NULL;
}

Node* NodeMgr::OpenNode(cluster_t n)
{
    Node* node = NULL;
    ClusterContainer* cluster = cluster_service->Fetch(n);
    ASSERT_NULL(cluster);

    node = new Node(this, cluster);
    ASSERT_NULL(node);

    ASSERT_FALSE(node->Load());

    return node;
faild:
    DELETE(node);
    if (cluster != NULL)
    {
        cluster_service->Dispose(*cluster);
    }
    return NULL;
}

bool NodeMgr::Close(Node* node)
{
    bool ret = true;

    ret = node->Sync() && ret;
    ret = cluster_service->Dispose(*node->current) && ret;
    ret = cluster_service->Dispose(*node->MC) && ret;

    delete node;

    return ret;
}

bool NodeMgr::Delete(Node* node)
{
    cluster_t mc = node->MC->GetCluster();

    ASSERT_FALSE(node->Truncate(0)); // �Ƚ��ļ��ߴ���СΪ0�����ͷ����õ����ݴ�
    ASSERT_FALSE(Close(node)); // �ٹر� node
    ASSERT_FALSE(cluster_service->Free(mc));// ����ͷŵ� MC ռ�õĴ�

    return true;
faild:
    return false;
}

bool NodeMgr::Sync()
{
    return false;
}
