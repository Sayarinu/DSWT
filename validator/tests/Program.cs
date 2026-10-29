using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Net.Client;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using PKHeX.Core;
using PlatinumWT.Validator;
using Platinumwt.V1;

if (args.Length == 2 && args[0] == "--audit-image")
{
    ImageAudit.Run(args[1]);
    return;
}

int passed = 0;
void Require(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
async Task Test(string name, Func<Task> test)
{
    await test();
    passed++;
    Console.WriteLine($"PASS {name}");
}
void CheckReply(ValidateReply reply, byte[] data)
{
    Require(reply.PolicyVersion == Policy.Version, "policy mismatch");
    Require(reply.Sha256.Span.SequenceEqual(SHA256.HashData(data)), "payload mismatch");
    Require(reply.ReasonCodes.Count <= 8 && reply.ReasonCodes.All(r => r.Length is > 0 and <= 32 && r.All(c => c is >= 'A' and <= 'Z' or '_')), "reason bounds");
    Require(reply.Legal == (reply.ReasonCodes.Count == 0), "ambiguous verdict");
}
byte[] Canonical(PK4 pk)
{
    pk.RefreshChecksum();
    return pk.Data[..Policy.PayloadSize].ToArray();
}
byte[] Mutate(byte[] original, Action<PK4> change)
{
    var pk = new PK4(original.ToArray());
    change(pk);
    return Canonical(pk);
}
async Task ExpectStatus(Func<Task> action, params StatusCode[] expected)
{
    try { await action(); }
    catch (RpcException error) { Require(expected.Contains(error.StatusCode), $"unexpected status {error.StatusCode}"); return; }
    throw new InvalidOperationException("expected failure");
}

// Synthetic trainer and generated encounters only. No save, ROM or upstream
// binary Pokémon fixture is copied into the source tree or test output.
var trainer = new SimpleTrainerInfo(GameVersion.Pt) { OT = "VALIDATOR", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
var encounters = EncounterMovesetGenerator.GenerateEncounters(new PK4 { Species = 387 }, trainer,
    ReadOnlyMemory<ushort>.Empty, [GameVersion.Pt]);
var starterEncounter = encounters.OfType<EncounterStatic4>().First(e => !e.IsEgg);
var starter = starterEncounter.ConvertToPKM(trainer);
starter.MetDate = new DateOnly(2020, 1, 1);
var good = Canonical(starter);
Require(starter.Species == 387 && starter.Version == GameVersion.Pt && starter.MetLevel == 5, "unexpected fixture encounter");

byte[] Wild()
{
    var encounter = EncounterMovesetGenerator.GenerateEncounters(new PK4 { Species = 399 }, trainer,
        ReadOnlyMemory<ushort>.Empty, [GameVersion.Pt]).OfType<EncounterSlot4>().First();
    return Canonical((PK4)encounter.ConvertToPKM(trainer));
}
if (args.Length != 0)
{
    Require(args.Length == 2 && args[0] == "--export-fixtures", "use --export-fixtures PRIVATE_EMPTY_DIRECTORY");
    var directory = new DirectoryInfo(args[1]);
    Require(directory.Exists && !directory.EnumerateFileSystemInfos().Any()
        && directory.LinkTarget is null, "fixture directory must already exist, empty and unlinked");
    if (!OperatingSystem.IsWindows())
        Require((File.GetUnixFileMode(directory.FullName) & (UnixFileMode.GroupRead | UnixFileMode.GroupWrite
            | UnixFileMode.GroupExecute | UnixFileMode.OtherRead | UnixFileMode.OtherWrite | UnixFileMode.OtherExecute)) == 0,
            "fixture directory must be private");
    var fixtures = new Dictionary<string, byte[]> {
        ["starter.pk4"] = good, ["wild.pk4"] = Wild(),
        ["illegal-rng.pk4"] = Mutate(good, pk => pk.IV32 ^= 0x3FFFFFFF),
    };
    var hashes = new Dictionary<string, string>();
    foreach (var (name, data) in fixtures)
    {
        var verdict = Policy.Analyze(data);
        CheckReply(verdict, data);
        Require(name == "illegal-rng.pk4" ? !verdict.Legal && verdict.ReasonCodes.Contains("ILLEGAL_RNG") : verdict.Legal,
            "generated fixture verdict mismatch");
        var options = new FileStreamOptions { Mode = FileMode.CreateNew, Access = FileAccess.Write };
        if (!OperatingSystem.IsWindows()) options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        using var output = new FileStream(Path.Combine(directory.FullName, name), options);
        output.Write(data); output.Flush(true);
        hashes[name] = Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();
    }
    var metadata = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(new {
        version = 1, policy = Policy.Version, source = "generated pinned PKHeX test encounters", sha256 = hashes,
    });
    using (var output = new FileStream(Path.Combine(directory.FullName, "manifest.json"), FileMode.CreateNew))
    { output.Write(metadata); output.Flush(true); }
    Console.WriteLine("Exported three generated test fixtures; no ROM or save inputs");
    return;
}

await Test("legal Platinum starter and unchanged canonical input", () =>
{
    byte[] before = good.ToArray();
    var reply = Policy.Analyze(good);
    CheckReply(reply, good);
    Require(reply.Legal, "generated starter rejected: " + string.Join(',', reply.ReasonCodes));
    Require(good.SequenceEqual(before), "input was changed");
    return Task.CompletedTask;
});

await Test("legal generated wild encounter", () =>
{
    var data = Wild();
    var reply = Policy.Analyze(data);
    CheckReply(reply, data);
    Require(reply.Legal, "generated wild encounter rejected");
    return Task.CompletedTask;
});

await Test("Gen IV names are not retroactively censored and data stays strict", () =>
{
    const string name = "SHIT";
    ParseSettings.Initialize(new LegalitySettings());
    Require(WordFilter.IsFiltered(name, EntityContext.Gen4, out _, out _), "fixture must trigger the default modern filter");
    var namedTrainer = new SimpleTrainerInfo(GameVersion.Pt) { OT = name, TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
    var pk = (PK4)starterEncounter.ConvertToPKM(namedTrainer);
    pk.Nickname = name; pk.IsNicknamed = true; pk.MetDate = new DateOnly(2020, 1, 1);
    var data = Canonical(pk); var before = data.ToArray();
    Require(Policy.Analyze(data).Legal && data.SequenceEqual(before), "historical names rejected or repaired");
    var invalid = Mutate(data, value => value.IV32 ^= 0x3FFFFFFF);
    Require(!Policy.Analyze(invalid).Legal, "name exception bypassed RNG checks");
    return Task.CompletedTask;
});

await Test("event rules accept generated gift and reject altered encounter", () =>
{
    var gift = EncounterEvent.MGDB_G4.First(g => g.Species == 491 && !g.IsEgg && g.IsEntity);
    var data = Canonical((PK4)gift.ConvertToPKM(trainer));
    Require(Policy.Analyze(data).Legal, "generated event rejected");
    var altered = Mutate(data, pk => pk.FatefulEncounter = !pk.FatefulEncounter);
    Require(!Policy.Analyze(altered).Legal, "altered event accepted");
    return Task.CompletedTask;
});

await Test("legal Gen III transfer remains compatible", () =>
{
    var origin = new SimpleTrainerInfo(GameVersion.R) { OT = "VALIDATOR", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
    var encounter = EncounterMovesetGenerator.GenerateEncounters(new PK3 { Species = 252 }, origin,
        ReadOnlyMemory<ushort>.Empty, [GameVersion.R]).OfType<EncounterStatic3>().First(e => !e.IsEgg);
    var data = Canonical(((PK3)encounter.ConvertToPKM(origin)).ConvertToPK4());
    var reply = Policy.Analyze(data);
    CheckReply(reply, data);
    Require(reply.Legal, "generated Gen III transfer rejected");
    return Task.CompletedTask;
});

var illegal = new (string Name, byte[] Data, string Reason)[]
{
    ("egg", Mutate(good, pk => pk.IsEgg = true), "EGG_NOT_ALLOWED"),
    ("mail", Mutate(good, pk => pk.HeldItem = 137), "MAIL_NOT_ALLOWED"),
    ("future species", Mutate(good, pk => pk.Species = 494), "INVALID_SPECIES"),
    ("empty species", Mutate(good, pk => pk.Species = 0), "INVALID_SPECIES"),
    ("invalid form", Mutate(good, pk => pk.Form = 31), "INCOMPATIBLE_FORM"),
    ("spiky Pichu", Mutate(good, pk => { pk.Species = 172; pk.Form = 1; }), "INCOMPATIBLE_FORM"),
    ("future move", Mutate(good, pk => pk.Move1 = 548), "ILLEGAL_MOVES"),
    ("impossible encounter", Mutate(good, pk => pk.MetLevel = 0), "ILLEGAL_ENCOUNTER"),
    ("invalid EV sum", Mutate(good, pk => { pk.EV_HP = 255; pk.EV_ATK = 255; pk.EV_DEF = 255; }), "ILLEGAL_DATA"),
    ("invalid RNG correlation", Mutate(good, pk => pk.IV32 ^= 0x3FFFFFFF), "ILLEGAL_RNG"),
};
foreach (var fixture in illegal)
    await Test(fixture.Name, () =>
    {
        var reply = Policy.Analyze(fixture.Data);
        CheckReply(reply, fixture.Data);
        Require(!reply.Legal && reply.ReasonCodes.Contains(fixture.Reason), "missing expected reason: " + fixture.Name);
        return Task.CompletedTask;
    });

await Test("checksum sanity and encryption sentinel fail without repair", () =>
{
    foreach (int offset in new[] { 4, 6, 0x64 })
    {
        var malformed = good.ToArray();
        malformed[offset] ^= 1;
        if (offset == 0x64)
        {
            uint sum = 0;
            for (int i = 8; i < malformed.Length; i += 2) sum += BinaryPrimitives.ReadUInt16LittleEndian(malformed.AsSpan(i));
            BinaryPrimitives.WriteUInt16LittleEndian(malformed.AsSpan(6), (ushort)sum);
        }
        byte[] before = malformed.ToArray();
        var reply = Policy.Analyze(malformed);
        Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(new[] { "INVALID_STRUCTURE" }), "malformed input accepted");
        Require(malformed.SequenceEqual(before), "malformed input was repaired");
    }
    return Task.CompletedTask;
});

await Test("real isolated worker accepts legal and rejects illegal payload", async () =>
{
    using var runner = new WorkerRunner();
    var clock = Stopwatch.StartNew();
    var reply = await runner.Run(good, CancellationToken.None);
    CheckReply(reply, good);
    Require(reply.Legal, "worker rejected legal input");
    Console.WriteLine($"Worker cold validation: {clock.ElapsedMilliseconds} ms");
    reply = await runner.Run(illegal[0].Data, CancellationToken.None);
    Require(!reply.Legal, "worker accepted egg");
});

ProcessStartInfo Shell(string command)
{
    var start = new ProcessStartInfo("/bin/sh");
    start.ArgumentList.Add("-c");
    start.ArgumentList.Add(command);
    return start;
}
await Test("worker deadline kills hung process and releases admission", async () =>
{
    string path = Path.Combine(Path.GetTempPath(), "validator-test-" + Guid.NewGuid().ToString("N"));
    using var runner = new WorkerRunner(() =>
    {
        var start = Shell("echo $$ > \"$1\"; exec sleep 20");
        start.ArgumentList.Add("fixture");
        start.ArgumentList.Add(path);
        return start;
    }, TimeSpan.FromMilliseconds(200));
    try
    {
        await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.Unavailable);
        int pid = int.Parse(await File.ReadAllTextAsync(path));
        bool alive;
        try { using var process = Process.GetProcessById(pid); alive = !process.HasExited; }
        catch (ArgumentException) { alive = false; }
        Require(!alive, "timed-out worker survived");
    }
    finally { File.Delete(path); }
});

await Test("worker capacity is two with no waiting queue", async () =>
{
    using var runner = new WorkerRunner(() => Shell("exec sleep 20"), TimeSpan.FromMilliseconds(200));
    var first = runner.Run(good, CancellationToken.None);
    var second = runner.Run(good, CancellationToken.None);
    await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.ResourceExhausted);
    await ExpectStatus(async () => await first, StatusCode.Unavailable);
    await ExpectStatus(async () => await second, StatusCode.Unavailable);
});

await Test("configured worker capacity is bounded and still has no waiting queue", async () =>
{
    Require(WorkerRunner.Workers(null) == 2 && WorkerRunner.Workers("") == 2 && WorkerRunner.Workers("4") == 4, "worker count");
    foreach (var invalid in new[] { "0", "9", "12", "x", " 4" })
    {
        try { WorkerRunner.Workers(invalid); Require(false, "accepted worker count " + invalid); }
        catch (InvalidOperationException) { }
    }
    using var runner = new WorkerRunner(() => Shell("exec sleep 20"), TimeSpan.FromMilliseconds(200), 3);
    var running = Enumerable.Range(0, 3).Select(_ => runner.Run(good, CancellationToken.None)).ToArray();
    await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.ResourceExhausted);
    foreach (var item in running)
        await ExpectStatus(async () => await item, StatusCode.Unavailable);
});

await Test("worker cancellation crash malformed output and floods are unavailable", async () =>
{
    foreach (var command in new[] { "exit 2", "exit 0", "printf invalid", "head -c 4096 /dev/zero" })
    {
        using var runner = new WorkerRunner(() => Shell(command), TimeSpan.FromMilliseconds(200));
        await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.Unavailable);
    }
    using var hung = new WorkerRunner(() => Shell("exec sleep 20"), TimeSpan.FromSeconds(1));
    using var cancellation = new CancellationTokenSource(30);
    await ExpectStatus(async () => await hung.Run(good, cancellation.Token), StatusCode.Cancelled);
});

await Test("actual internal gRPC payload verdict deadline and size contract", async () =>
{
    await using var app = ServiceHost.Build(0);
    await app.StartAsync();
    var addresses = app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!;
    Require(addresses.Addresses.Count == 1 && addresses.Addresses.Single().StartsWith("http://127.0.0.1:", StringComparison.Ordinal), "non-loopback endpoint");
    using var channel = GrpcChannel.ForAddress(addresses.Addresses.Single());
    var client = new PokemonLegality.PokemonLegalityClient(channel);
    ValidateRequest Request(byte[] bytes) => new() { Pokemon = new() { Pk4 = ByteString.CopyFrom(bytes), Sha256 = ByteString.CopyFrom(SHA256.HashData(bytes)) } };
    var reply = await client.ValidateAsync(Request(good), deadline: DateTime.UtcNow.AddSeconds(2));
    CheckReply(reply, good);
    Require(reply.Legal, "gRPC rejected legal payload");
    var wrongHash = Request(good);
    wrongHash.Pokemon.Sha256 = ByteString.CopyFrom(new byte[32]);
    await ExpectStatus(async () => await client.ValidateAsync(wrongHash), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(new ValidateRequest()), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[135])), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[260])), StatusCode.ResourceExhausted);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[4096])), StatusCode.Unknown);
    await ExpectStatus(async () => await client.ValidateAsync(Request(good), deadline: DateTime.UtcNow.AddMilliseconds(1)), StatusCode.DeadlineExceeded);
    await app.StopAsync();
});
Console.WriteLine($"{passed} validator scenarios passed.");
