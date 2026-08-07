// Smoke test — proves the harness is wired correctly: GoogleTest fetched +
// compiled + linked, gtest_discover_tests() enumerated the tests, ctest ran
// them, and assertions fire. This is NOT coverage of any firmware unit.
//
// Replace / extend with real coverage in a follow-up session.
//
// Convention: one *_test.cpp per unit under test. Each TEST() becomes its own
// ctest entry. Prefer TEST_F fixtures for shared setup. See tests/README.md.

#include <gtest/gtest.h>

namespace {

// Proof of life: the framework executed and EXPECT/ASSERT work.
TEST(Smoke, BasicAssertionWorks) {
    EXPECT_EQ(1 + 1, 2);
    ASSERT_TRUE(true);
}

// Template fixture for future coverage sessions — copy this pattern when a
// unit needs shared per-test setup/teardown.
class ExampleFixture : public ::testing::Test {
protected:
    void SetUp() override {
        some_shared_state_ = 42;
    }
    int some_shared_state_ = 0;
};

TEST_F(ExampleFixture, DemonstratesFixtureSetup) {
    EXPECT_EQ(some_shared_state_, 42);
}

}  // namespace
