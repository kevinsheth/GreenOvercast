const std = @import("std");

pub fn selectDefaultBaseUri(data: []const u8, output: []u8) !usize {
    const parsed = try std.json.parseFromSlice(std.json.Value, std.heap.page_allocator, data, .{});
    defer parsed.deinit();

    const root = switch (parsed.value) {
        .object => |object| object,
        else => return error.MissingOfferingSettings,
    };
    const settings = switch (root.get("offeringSettings") orelse return error.MissingOfferingSettings) {
        .object => |object| object,
        else => return error.MissingOfferingSettings,
    };
    const regions = switch (settings.get("regions") orelse return error.MissingRegions) {
        .array => |array| array,
        else => return error.MissingRegions,
    };

    for (regions.items) |region| {
        const object = switch (region) {
            .object => |value| value,
            else => continue,
        };
        const is_default = switch (object.get("isDefault") orelse continue) {
            .bool => |value| value,
            else => continue,
        };
        if (!is_default) continue;
        const configured_uri = switch (object.get("baseUri") orelse return error.MissingBaseUri) {
            .string => |value| value,
            else => return error.MissingBaseUri,
        };
        const uri = std.mem.trimRight(u8, configured_uri, "/");
        if (!std.mem.startsWith(u8, uri, "https://") or uri.len == "https://".len)
            return error.InvalidBaseUri;
        if (uri.len >= output.len) return error.NoSpaceLeft;
        @memcpy(output[0..uri.len], uri);
        output[uri.len] = 0;
        return uri.len;
    }
    return error.MissingDefaultRegion;
}

test "selects the marked default region" {
    const response =
        \\{"offeringSettings":{"regions":[
        \\  {"name":"WestEurope","baseUri":"https://weu.example/","isDefault":false},
        \\  {"name":"EastUS","baseUri":"https://eus.example/","isDefault":true}
        \\]}}
    ;
    var output: [128]u8 = undefined;
    const length = try selectDefaultBaseUri(response, &output);
    try std.testing.expectEqualStrings("https://eus.example", output[0..length]);
    try std.testing.expectEqual(@as(u8, 0), output[length]);
}

test "rejects missing and invalid default regions" {
    var output: [32]u8 = undefined;
    try std.testing.expectError(
        error.MissingDefaultRegion,
        selectDefaultBaseUri(
            \\{"offeringSettings":{"regions":[{"baseUri":"https://weu.example","isDefault":false}]}}
        , &output),
    );
    try std.testing.expectError(
        error.InvalidBaseUri,
        selectDefaultBaseUri(
            \\{"offeringSettings":{"regions":[{"baseUri":"http://eus.example","isDefault":true}]}}
        , &output),
    );
}

test "rejects oversized region URIs" {
    var output: [16]u8 = undefined;
    try std.testing.expectError(
        error.NoSpaceLeft,
        selectDefaultBaseUri(
            \\{"offeringSettings":{"regions":[{"baseUri":"https://eastus.example","isDefault":true}]}}
        , &output),
    );
}
