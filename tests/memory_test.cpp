#include "nordend/core/memory.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <doctest/doctest.h>

TEST_CASE("memory report: the process, malloc and the lines for the log") {
    const nordend::core::MemoryReport before = nordend::core::read_memory_report();
    CHECK(before.rss > 0);
    CHECK(before.anonymous > 0);
    CHECK(before.anonymous <= before.rss);
    REQUIRE_FALSE(before.mappings.empty());
    for (std::size_t i = 1; i < before.mappings.size(); ++i) {
        CHECK(before.mappings[i - 1].rss >= before.mappings[i].rss);  // largest first
    }

    // 64 MB held and touched: malloc and the process both see it.
    constexpr std::size_t size = std::size_t{64} << 20U;
    std::vector<std::uint8_t> block(size, 1);
    const nordend::core::MemoryReport during = nordend::core::read_memory_report();
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    // Under a sanitizer malloc is the sanitizer's: glibc's mallinfo2 reports nothing.
    CHECK(during.heap_in_use >= before.heap_in_use + size);
    CHECK(during.anonymous >= before.anonymous + size);
#else
    // The sanitizer's allocator gives freed memory back on its own, between the two reads too
    // (after the whole suite it holds some 500 MB): the block shows, not to the byte.
    CHECK(during.anonymous >= before.anonymous + (size / 2));
#endif

    const std::vector<std::string> lines = nordend::core::describe(during);
    REQUIRE(lines.size() >= 3);
    CHECK(lines[0].starts_with("process: rss "));
    CHECK(lines[1].starts_with("malloc: in use "));
    CHECK(lines[2].starts_with("gpu: "));  // "?" without a GPU client in the tests
    CHECK(block.back() == 1);
}

TEST_CASE("memory: large blocks go back to the system once freed") {
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
    nordend::core::give_back_large_blocks();
    const nordend::core::MemoryReport before = nordend::core::read_memory_report();
    for (int i = 0; i < 8; ++i) {
        // 20 MB, as a decoded photo preview: over the threshold, so a mapping of its own.
        std::vector<std::uint8_t> block(std::size_t{20} << 20U, static_cast<std::uint8_t>(i));
        CHECK(block.back() == i);
    }
    const nordend::core::MemoryReport after = nordend::core::read_memory_report();
    // malloc's own counters, not the process's anonymous memory: another thread still running
    // from an earlier test (Mesa's software renderer keeps some) may allocate meanwhile.
    CHECK(after.heap_free < before.heap_free + (std::size_t{4} << 20U));    // nothing kept free
    CHECK(after.heap_mmapped <= before.heap_mmapped);                       // each unmapped
    CHECK(after.heap_arena < before.heap_arena + (std::size_t{4} << 20U));  // none in the heap
#endif
}
