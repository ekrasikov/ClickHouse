#include <Storages/MarkCache.h>

namespace DB
{

struct MergeTreeIndexGranularityInfo;

class MergeTreeMarksLoader
{
public:
    using MarksPtr = MarkCache::MappedPtr;

    MergeTreeMarksLoader(
        MarkCache * mark_cache_,
        const String & mrk_path,
        size_t marks_count_,
        const MergeTreeIndexGranularityInfo & index_granularity_info_,
        bool save_marks_in_cache_,
        size_t columns_num_in_mark_ = 1);

    const MarkInCompressedFile & getMark(size_t row_index, size_t column_index = 0);

    bool initialized() const { return marks != nullptr; }

private:
    MarkCache * mark_cache = nullptr;
    String mrk_path;
    size_t marks_count;
    const MergeTreeIndexGranularityInfo & index_granularity_info;
    bool save_marks_in_cache = false;
    size_t columns_num_in_mark_;
    MarkCache::MappedPtr marks;

    void loadMarks();
    MarkCache::MappedPtr loadMarksImpl();
};

}
