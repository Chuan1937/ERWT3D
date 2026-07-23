#include "erwt3d/ssd/ssd_rzfp_executor.hpp"

namespace erwt3d {

bool executeRzfpRoundSSD(
    int /*fd*/,
    const RzfpFileHeader& /*header*/,
    const std::vector<RzfpSuperblockIndex>& /*sbIndex*/,
    const std::vector<RzfpLeafDescriptor>& /*descriptors*/,
    const RzfpRoundPlan& /*roundPlan*/,
    const std::vector<RzfpReader::ContestRoundGroup>& /*groups*/,
    const SSDReadConfig& /*ssdCfg*/,
    std::vector<RzfpReader::RzfpRoundReadResult>* /*results*/,
    RzfpSSDExecProfile* /*profile*/)
{
    return false;
}

} // namespace erwt3d
