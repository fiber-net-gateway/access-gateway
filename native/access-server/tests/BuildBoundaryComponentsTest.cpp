#include <gtest/gtest.h>

#include <type_traits>

#include "routing/AccessRouteSnapshot.h"
#include "routing/AccessScriptCompiler.h"
#include "runtime/AccessScriptRuntime.h"

namespace {

using fiber::access_server::AccessRouteSnapshotProvider;
using fiber::access_server::AccessScriptCompiler;
using fiber::access_server::AccessScriptRuntime;

TEST(BuildBoundaryComponentsTest, KeepsCompilationStateOutOfRequestRuntime) {
    EXPECT_FALSE(std::is_polymorphic_v<AccessScriptCompiler>);
    EXPECT_FALSE(std::is_polymorphic_v<AccessScriptRuntime>);
    EXPECT_FALSE(std::is_empty_v<AccessScriptCompiler>);
    EXPECT_TRUE(std::is_empty_v<AccessScriptRuntime>);
}

TEST(BuildBoundaryComponentsTest, KeepsSnapshotProviderAsAValueAdapter) {
    EXPECT_TRUE(std::is_trivially_copyable_v<AccessRouteSnapshotProvider>);
    EXPECT_LE(sizeof(AccessRouteSnapshotProvider), 2U * sizeof(void *));
}

} // namespace
