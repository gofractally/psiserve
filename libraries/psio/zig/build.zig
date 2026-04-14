const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Library module
    const lib_mod = b.addModule("psio-wit", .{
        .root_source_file = b.path("src/encode.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Static library (optional artifact)
    const lib = b.addStaticLibrary(.{
        .name = "psio-wit",
        .root_module = lib_mod,
    });
    b.installArtifact(lib);

    // Tests
    const test_encode = b.addTest(.{
        .root_source_file = b.path("src/encode_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    const run_tests = b.addRunArtifact(test_encode);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_tests.step);
}
