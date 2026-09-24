const std = @import("std");

pub const default_kbps: u32 = 2_000;
pub const minimum_kbps: u32 = 500;
pub const maximum_kbps: u32 = 20_000;

pub fn parseKbps(value: ?[]const u8) !u32 {
    const text = value orelse return default_kbps;
    const bitrate = std.fmt.parseInt(u32, text, 10) catch return error.InvalidBitrate;
    if (bitrate < minimum_kbps or bitrate > maximum_kbps) return error.InvalidBitrate;
    return bitrate;
}

pub fn bitsPerSecond(kbps: u32) u32 {
    return kbps * 1_000;
}

pub fn measureKbps(bytes: u64, milliseconds: u32) u64 {
    if (milliseconds == 0) return 0;
    return bytes * 8 / milliseconds;
}

test "uses the conservative default when unset" {
    try std.testing.expectEqual(@as(u32, 2_000), try parseKbps(null));
}

test "accepts supported video bitrates" {
    try std.testing.expectEqual(@as(u32, 500), try parseKbps("500"));
    try std.testing.expectEqual(@as(u32, 8_000), try parseKbps("8000"));
    try std.testing.expectEqual(@as(u32, 20_000), try parseKbps("20000"));
    try std.testing.expectEqual(@as(u32, 8_000_000), bitsPerSecond(8_000));
}

test "measures received bitrate over an interval" {
    try std.testing.expectEqual(@as(u64, 8_000), measureKbps(1_000_000, 1_000));
    try std.testing.expectEqual(@as(u64, 0), measureKbps(1_000_000, 0));
}

test "rejects malformed and out of range video bitrates" {
    try std.testing.expectError(error.InvalidBitrate, parseKbps(""));
    try std.testing.expectError(error.InvalidBitrate, parseKbps("499"));
    try std.testing.expectError(error.InvalidBitrate, parseKbps("20001"));
    try std.testing.expectError(error.InvalidBitrate, parseKbps("fast"));
    try std.testing.expectError(error.InvalidBitrate, parseKbps("4294967296"));
}
