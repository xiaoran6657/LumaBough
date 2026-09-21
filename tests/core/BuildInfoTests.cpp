#include <MiniEngine/Core/BuildInfo.h>

#include <gtest/gtest.h>

#include <string_view>

TEST(BuildInfoTests, ReportsEngineName)
{
    EXPECT_EQ(MiniEngine::GetEngineName(), std::string_view{"MiniEngine"});
}