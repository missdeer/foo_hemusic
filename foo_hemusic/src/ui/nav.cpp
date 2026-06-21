#include "ui/nav.h"

#include <utility>

namespace hemusic::ui::nav {

void Stack::replaceRoot(PageEntry entry) {
    entries_.clear();
    entries_.push_back(std::move(entry));
}

void Stack::push(PageEntry entry) {
    if (entries_.empty()) {
        // push without a root is meaningless; treat it as replaceRoot so the
        // caller gets a usable state instead of a silent drop.
        entries_.push_back(std::move(entry));
        return;
    }
    entries_.push_back(std::move(entry));
}

std::optional<PageEntry> Stack::pop() {
    if (entries_.size() <= 1) {
        return std::nullopt;
    }
    PageEntry top = std::move(entries_.back());
    entries_.pop_back();
    return top;
}

}  // namespace hemusic::ui::nav
