#include "add.hpp"

#include <gtest/gtest.h>

TEST(AddTest, BasicSum) { EXPECT_EQ(starter::add(1, 2), 3); }

TEST(AddTest, NegativeNumbers) { EXPECT_EQ(starter::add(-1, -2), -3); }

TEST(AddTest, Zero) { EXPECT_EQ(starter::add(0, 0), 0); }
