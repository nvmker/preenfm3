/*
 * Phase 8.8 SW2 — MenuItemUtil lookup bounds (real-table host tests).
 *
 * Red→green contract: hardware/MenuTable.cpp carries the REAL allMenus[]
 * table. The ORIGINAL lookup implementation (moved verbatim from Menu.h)
 * terminated its scan on a LAST_MENU sentinel entry that does not exist in
 * the table — every MISS (and only a miss: a hit returns in-bounds at the
 * matching entry) read one entry past the array end. The absent-key tests
 * below trip ASAN
 * global-buffer-overflow against that implementation (verify via
 * `make test-asan` with the sentinel bodies restored) and pass against the
 * ARRAY_SIZE-bounded walks.
 *
 * The sweeps also pin the table invariants the firmware relies on:
 *  - every constant-key consumer (FMDisplayMenu MENU_ERROR / MENU_IN_PROGRESS
 *    sites, SynthState boot MAIN_MENU) resolves non-null — the guards'
 *    fallback target must exist;
 *  - every subMenu value referenced by any entry resolves (navigation
 *    closure; unused slots are 0 = MAIN_MENU, which is entry 0);
 *  - menuState values are unique (getMenuItem returns the FIRST match, so a
 *    duplicate entry would silently shadow navigation).
 */
#include "Menu.h"

#include <gtest/gtest.h>

#include <set>

namespace {

TEST(MenuLookup, GuardReliedStatesResolve) {
    // The states constant-key consumers assign unguarded; MENU_ERROR is also
    // the fallback target of the FMDisplayMenu guards.
    const MenuState pinned[] = {MAIN_MENU, MENU_ERROR, MENU_IN_PROGRESS,
                                MENU_DONE,  MENU_CANCEL,
                                MENU_PRESET_LOAD_SELECT_DX7_BANK};
    for (MenuState ms : pinned) {
        const MenuItem* item = MenuItemUtil::getMenuItem(ms);
        ASSERT_NE(item, nullptr) << "state " << static_cast<int>(ms)
                                 << " must have a table entry";
        EXPECT_EQ(item->menuState, ms);
    }
}

TEST(MenuLookup, AbsentStateReturnsNullWithinBounds) {
    // LAST_MENU is the enum terminator: it is (and must remain) absent from
    // the table — the former sentinel walk ran past the array reading for it.
    EXPECT_EQ(MenuItemUtil::getMenuItem(LAST_MENU), nullptr);
    // Valid-but-unenumerated keys must also miss cleanly. NOTE: cast values
    // must stay INSIDE MenuState's valid value range (0..63 — the smallest
    // bit-field covering enumerators 0..LAST_MENU). 0x7f/127 is OUT of range
    // and merely forming/comparing it is UB (UBSan "not a valid value for
    // type MenuState", review round finding); 63 is the max in-range probe.
    EXPECT_EQ(MenuItemUtil::getMenuItem(static_cast<MenuState>(LAST_MENU + 1)),
              nullptr);
    EXPECT_EQ(MenuItemUtil::getMenuItem(static_cast<MenuState>(63)), nullptr);
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(LAST_MENU), nullptr);
}

TEST(MenuLookup, TableStatesResolveAndRoundTrip) {
    ASSERT_GT(MenuItemUtil::menuCount(), 0);
    for (int i = 0; i < MenuItemUtil::menuCount(); i++) {
        const MenuState ms = allMenus[i].menuState;
        const MenuItem* item = MenuItemUtil::getMenuItem(ms);
        ASSERT_NE(item, nullptr) << "table entry " << i << " (state "
                                 << static_cast<int>(ms) << ") must resolve";
        EXPECT_EQ(item->menuState, ms);
    }
}

TEST(MenuLookup, MenuStatesAreUnique) {
    std::set<int> seen;
    for (int i = 0; i < MenuItemUtil::menuCount(); i++) {
        int ms = static_cast<int>(allMenus[i].menuState);
        EXPECT_TRUE(seen.insert(ms).second)
            << "duplicate menuState " << ms << " at entry " << i
            << " — getMenuItem would silently return only the first";
    }
}

TEST(MenuLookup, EverySubMenuValueResolves) {
    // Navigation closure: each subMenu id (incl. the MENU_DONE/MENU_CANCEL
    // terminals and the 0 = MAIN_MENU padding of unused slots) must be a real
    // table entry, or FMDisplayMenu's subMenu[key] lookups would miss.
    for (int i = 0; i < MenuItemUtil::menuCount(); i++) {
        for (int k = 0; k < 6; k++) {
            MenuState sub = allMenus[i].subMenu[k];
            EXPECT_NE(MenuItemUtil::getMenuItem(sub), nullptr)
                << "subMenu[" << k << "] of entry " << i << " (state "
                << static_cast<int>(allMenus[i].menuState)
                << ") does not resolve: " << static_cast<int>(sub);
        }
    }
}

TEST(MenuLookup, ParentLookupSemanticsPreserved) {
    // Terminal states return themselves (blocks the back button); MAIN_MENU
    // has no parent; a real child returns its parent.
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(MENU_DONE),
              MenuItemUtil::getMenuItem(MENU_DONE));
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(MENU_CANCEL),
              MenuItemUtil::getMenuItem(MENU_CANCEL));
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(MENU_ERROR),
              MenuItemUtil::getMenuItem(MENU_ERROR));
    // Unused subMenu slots are zero-initialized and MAIN_MENU == 0, so the
    // first row's padding slot matches: MAIN_MENU's "parent" is ITSELF
    // (&allMenus[0]) — pressing back from the top stays at the top. The
    // original sentinel walk matched the same slot at the same scan order;
    // the bounded walk preserves that exactly (verified in-bounds, and null
    // is still returned for states no subMenu references, e.g. LAST_MENU).
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(MAIN_MENU),
              MenuItemUtil::getMenuItem(MAIN_MENU));
    EXPECT_EQ(MenuItemUtil::getParentMenuItem(MENU_MIXER),
              MenuItemUtil::getMenuItem(MAIN_MENU));
}

TEST(MenuLookup, EveryTableStateHasAParent) {
    // Reverse closure: getMenuBack/newMenuState (FMDisplayMenu.cpp:856/:909)
    // dereference getParentMenuItem(currentState) directly — every state that
    // can BE current (i.e. every table entry) must resolve to a parent entry
    // (or itself via the zero-padding / terminal special cases). This pins
    // the totality the display-side guards fall back from.
    for (int i = 0; i < MenuItemUtil::menuCount(); i++) {
        const MenuState ms = allMenus[i].menuState;
        EXPECT_NE(MenuItemUtil::getParentMenuItem(ms), nullptr)
            << "state " << static_cast<int>(ms) << " has no parent entry";
    }
}

}  // namespace
