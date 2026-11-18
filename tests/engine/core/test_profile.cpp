// SPDX-License-Identifier: Apache-2.0
// The phase profiler (profile.h): nothing recorded while it is off, a scope counted while it is on.

#include <gtest/gtest.h>

#include "groomlab/profile.h"

namespace groomlab {
namespace {

// The state the process had (GROOMLAB_PROFILE may be set for a whole test run), put back after.
struct ProfileState {
    bool was = profileEnabled();
    ~ProfileState() {
        profileReset();
        setProfileEnabled(was);
    }
};

TEST(Profile, OffRecordsNothing) {
    const ProfileState restore;
    setProfileEnabled(false);
    profileReset();
    {
        const ProfileScope scope("test.off");
    }
    profileAdd("test.off", 1.0);
    EXPECT_TRUE(profileReport().empty());
    EXPECT_TRUE(profileReportText().empty());
}

TEST(Profile, OnCountsScopesByLabel) {
    const ProfileState restore;
    setProfileEnabled(true);
    profileReset();
    for (int i = 0; i < 3; ++i) {
        const ProfileScope outer("test.outer");
        ProfileScope inner("test.inner");
        inner.stop();
        inner.stop();  // once only
    }
    profileAdd("test.added", 2.5);
    const auto rows = profileReport();
    ASSERT_EQ(rows.size(), 3u);
    // First-recorded order: the inner scope ends before the outer one.
    EXPECT_EQ(rows[0].label, "test.inner");
    EXPECT_EQ(rows[0].calls, 3u);
    EXPECT_EQ(rows[1].label, "test.outer");
    EXPECT_EQ(rows[1].calls, 3u);
    EXPECT_GE(rows[1].milliseconds, rows[0].milliseconds);
    EXPECT_EQ(rows[2].label, "test.added");
    EXPECT_DOUBLE_EQ(rows[2].milliseconds, 2.5);
    EXPECT_NE(profileReportText().find("test.added: 2.5 ms (1 call)"), std::string::npos);

    // A scope that starts after it went off is not counted.
    setProfileEnabled(false);
    {
        const ProfileScope scope("test.late");
    }
    EXPECT_EQ(profileReport().size(), 3u);
    profileReset();
    EXPECT_TRUE(profileReport().empty());
}

}  // namespace
}  // namespace groomlab
