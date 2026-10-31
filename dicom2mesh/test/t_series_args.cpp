#include "dicom2mesh.h"
#include <gtest/gtest.h>

TEST(ArgumentParser, SelectsSeriesAndConfiguresHeadOutline) {
    const char *args[] = {"-i",         "input", "--series",      "1.2.3", "--head-outline",
                          "--close-mm", "2",     "--resample-mm", "1"};
    const auto [ok, params] = Dicom2Mesh::parseCmdLineParameters(9, args);
    ASSERT_TRUE(ok);
    ASSERT_TRUE(params.seriesUid);
    EXPECT_EQ(*params.seriesUid, "1.2.3");
    EXPECT_TRUE(params.headOutline);
    EXPECT_EQ(params.closingRadiusMm, 2);
    EXPECT_EQ(params.resampleSpacing, 1);
}
TEST(ArgumentParser, ListsSeriesWithoutExport) {
    const char *args[] = {"-i", "input", "--list-series"};
    const auto [ok, params] = Dicom2Mesh::parseCmdLineParameters(3, args);
    ASSERT_TRUE(ok);
    EXPECT_TRUE(params.listSeries);
    EXPECT_FALSE(params.outputFilePath);
}
TEST(ArgumentParser, RejectsMissingAndInvalidGeometryOptions) {
    for (const auto *option : {"--series", "--close-mm", "--resample-mm"}) {
        const char *args[] = {"-i", "input", option};
        EXPECT_FALSE(std::get<0>(Dicom2Mesh::parseCmdLineParameters(3, args)));
    }
    for (const auto *value : {"-1", "nan", "abc", "0.01", "1mm"}) {
        const char *args[] = {"-i", "input", "--resample-mm", value};
        EXPECT_FALSE(std::get<0>(Dicom2Mesh::parseCmdLineParameters(4, args)));
    }
}
