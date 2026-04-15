const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Encode tests
    const encode_mod = b.createModule(.{
        .root_source_file = b.path("src/encode_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_encode = b.addTest(.{
        .name = "encode_test",
        .root_module = encode_mod,
    });
    const run_encode_tests = b.addRunArtifact(test_encode);

    // Fracpack tests
    const fracpack_mod = b.createModule(.{
        .root_source_file = b.path("src/fracpack.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_fracpack = b.addTest(.{
        .name = "fracpack_test",
        .root_module = fracpack_mod,
    });
    const run_fracpack_tests = b.addRunArtifact(test_fracpack);

    // Fracpack golden tests (all 71 test vectors)
    const golden_mod = b.createModule(.{
        .root_source_file = b.path("src/fracpack_golden_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_golden = b.addTest(.{
        .name = "fracpack_golden_test",
        .root_module = golden_mod,
    });
    const run_golden_tests = b.addRunArtifact(test_golden);

    // Mutation tests
    const mutation_mod = b.createModule(.{
        .root_source_file = b.path("src/mutation.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_mutation = b.addTest(.{
        .name = "mutation_test",
        .root_module = mutation_mod,
    });
    const run_mutation_tests = b.addRunArtifact(test_mutation);

    // Fracpack validation tests
    const validate_mod = b.createModule(.{
        .root_source_file = b.path("src/fracpack_validate_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_validate = b.addTest(.{
        .name = "fracpack_validate_test",
        .root_module = validate_mod,
    });
    const run_validate_tests = b.addRunArtifact(test_validate);

    // Fracpack JSON tests
    const json_mod = b.createModule(.{
        .root_source_file = b.path("src/fracpack_json_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_json = b.addTest(.{
        .name = "fracpack_json_test",
        .root_module = json_mod,
    });
    const run_json_tests = b.addRunArtifact(test_json);

    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_encode_tests.step);
    test_step.dependOn(&run_fracpack_tests.step);
    test_step.dependOn(&run_golden_tests.step);
    test_step.dependOn(&run_mutation_tests.step);
    test_step.dependOn(&run_validate_tests.step);
    test_step.dependOn(&run_json_tests.step);

    // Fracpack benchmarks (always ReleaseFast for meaningful results)
    const bench_mod = b.createModule(.{
        .root_source_file = b.path("src/bench.zig"),
        .target = target,
        .optimize = .ReleaseFast,
    });
    const bench_exe = b.addExecutable(.{
        .name = "fracpack_bench",
        .root_module = bench_mod,
    });
    bench_exe.linkLibC();
    b.installArtifact(bench_exe);
    const run_bench = b.addRunArtifact(bench_exe);
    const bench_step = b.step("bench", "Run fracpack benchmarks");
    bench_step.dependOn(&run_bench.step);
}
