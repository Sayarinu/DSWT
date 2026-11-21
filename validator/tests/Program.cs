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
using DSWT.Validator;
using Dswt.V1;

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
void CheckReply(ValidateReply reply, byte[] data, PokemonFormat format = PokemonFormat.FormatPk4)
{
    Require(reply.PolicyVersion == Policy.Version, "policy mismatch");
    Require(reply.Sha256.Span.SequenceEqual(SHA256.HashData(data)), "payload mismatch");
    Require(reply.ReasonCodes.Count <= 8 && reply.ReasonCodes.All(r => r.Length is > 0 and <= 32 && r.All(c => c is >= 'A' and <= 'Z' or '_')), "reason bounds");
    Require(reply.Legal == (reply.ReasonCodes.Count == 0), "ambiguous verdict");
    // Gen IV and Gen V are separate pools: a reply never carries the other format's bits.
    Require(reply.Legal == (reply.Destinations != 0) && (reply.Destinations & ~(uint)Policy.DestinationsFor(format)) == 0, "destination bounds");
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

byte[] NamedByTrainer(string name)
{
    var namedTrainer = new SimpleTrainerInfo(GameVersion.Pt) { OT = name, TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
    var pk = (PK4)starterEncounter.ConvertToPKM(namedTrainer);
    pk.MetDate = new DateOnly(2020, 1, 1);
    return Canonical(pk);
}
byte[] Nicknamed(string name) => Mutate(good, pk => { pk.Nickname = name; pk.IsNicknamed = true; });
void RequireName(byte[] data, bool allowed, string label)
{
    var reply = Policy.Analyze(data);
    CheckReply(reply, data);
    if (allowed)
        Require(reply.Legal, label + " was refused");
    else
        Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(["INAPPROPRIATE_NAME"]), label + " was not refused for its name");
}

await Test("names follow the Switch word list as a content rule, not legality", () =>
{
    var data = Nicknamed("SHIT"); var before = data.ToArray();
    RequireName(data, false, "offensive nickname");
    Require(data.SequenceEqual(before), "a refused name was repaired");
    RequireName(NamedByTrainer("SHIT"), false, "offensive trainer name");
    RequireName(Nicknamed("BULLSHIT"), false, "offensive compound");
    RequireName(Nicknamed("KILLER"), false, "blocked inflection");
    // An offensive name never hides an illegal Pokémon: both reasons are reported.
    var both = Mutate(Nicknamed("SHIT"), pk => pk.IV32 ^= 0x3FFFFFFF);
    var reply = Policy.Analyze(both);
    Require(!reply.Legal && reply.ReasonCodes.Contains("INAPPROPRIATE_NAME") && reply.ReasonCodes.Contains("ILLEGAL_RNG"), "name hid RNG check");
    return Task.CompletedTask;
});

await Test("names the list catches by accident stay tradable", () =>
{
    RequireName(good, true, "unnicknamed Pokémon");
    // Reviewed allow-list entries: given names and ordinary words.
    foreach (var name in new[] { "LULU", "MOSES", "Killian", "GRAPE", "COMPUTER", "Basement" })
        RequireName(Nicknamed(name), true, name);
    RequireName(NamedByTrainer("JODY"), true, "trainer JODY");
    // Official Pokémon terms in any Gen IV language, and species names.
    foreach (var name in new[] { "LEAFAGE", "SNATCH", "Coaching", "Osmose", "GIRATINA" })
        RequireName(Nicknamed(name), true, name);
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

ValidateReply ExpectLegal(byte[] data, Destination destinations)
{
    var reply = Policy.Analyze(data);
    CheckReply(reply, data);
    Require(reply.Legal, "legal fixture rejected: " + string.Join(',', reply.ReasonCodes));
    Require(reply.Destinations == (uint)destinations, $"destinations {(Destination)reply.Destinations}, expected {destinations}");
    return reply;
}
Destination DestinationsOf(byte[] data, Action<PK4> change) => Policy.Destinations(new PK4(Mutate(data, change)));

await Test("ordinary Pokémon fits every Gen IV game", () =>
{
    ExpectLegal(good, Policy.Gen4Destinations);
    ExpectLegal(Wild(), Policy.Gen4Destinations);
    return Task.CompletedTask;
});

await Test("Platinum-only forms do not fit Diamond and Pearl", () =>
{
    var giratina = EncounterMovesetGenerator.GenerateEncounters(new PK4 { Species = 487 }, trainer,
        ReadOnlyMemory<ushort>.Empty, [GameVersion.Pt]).OfType<EncounterStatic4>().First();
    var origin = (PK4)giratina.ConvertToPKM(trainer);
    origin.Form = 1;
    origin.HeldItem = 112; // Griseous Orb holds Origin Forme outside the Distortion World.
    origin.RefreshAbility(0); // Origin Forme has Levitate.
    ExpectLegal(Canonical(origin), Destination.Pt | Destination.HGSS);
    // Personal-table rules for the other Platinum forms, independent of encounter data.
    foreach (var (species, forms) in new (ushort, byte[])[] { (487, [1]), (492, [1]), (479, [1, 2, 3, 4, 5]) })
        foreach (var form in forms)
            Require(DestinationsOf(good, pk => { pk.Species = species; pk.Form = form; }) == (Destination.Pt | Destination.HGSS),
                $"species {species} form {form} destinations");
    return Task.CompletedTask;
});

await Test("held items follow each game's holdable items", () =>
{
    // Griseous Orb was added in Platinum.
    ExpectLegal(Mutate(good, pk => pk.HeldItem = 112), Destination.Pt | Destination.HGSS);
    // Fast Ball is an HG/SS-only item, but the pinned core holds HG/SS to
    // Platinum's holdable items, so it fits no game and is not legal.
    Require(DestinationsOf(good, pk => pk.HeldItem = 492) == Destination.None, "HG/SS item destinations");
    var reply = Policy.Analyze(Mutate(good, pk => pk.HeldItem = 492));
    Require(!reply.Legal && reply.Destinations == 0, "HG/SS-only held item accepted");
    return Task.CompletedTask;
});

await Test("HG/SS ball shows as the D/P/Pt ball byte", () =>
{
    var johto = new SimpleTrainerInfo(GameVersion.HG) { OT = "VALIDATOR", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
    var slot = EncounterMovesetGenerator.GenerateEncounters(new PK4 { Species = 16 }, johto,
        ReadOnlyMemory<ushort>.Empty, [GameVersion.HG]).OfType<EncounterSlot4>().First();
    var pk = (PK4)slot.ConvertToPKM(johto);
    pk.Ball = (byte)Ball.Fast;
    Require(pk.BallHGSS == (byte)Ball.Fast && pk.BallDPPt == (byte)Ball.Poke, "split ball fixture");
    var data = Canonical(pk);
    ExpectLegal(data, Policy.Gen4Destinations);
    // A D/P/Pt byte those games cannot show leaves only HG/SS.
    Require(DestinationsOf(data, value => value.BallDPPt = (byte)Ball.Fast) == Destination.HGSS, "invalid D/P/Pt ball byte");
    return Task.CompletedTask;
});

await Test("Spiky-eared Pichu fits no game", () =>
{
    Require(DestinationsOf(good, pk => { pk.Species = 172; pk.Form = 1; }) == Destination.None, "spiky Pichu destinations");
    Require(DestinationsOf(good, pk => { pk.Species = 172; pk.Form = 0; }) == Policy.Gen4Destinations, "Pichu destinations");
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
    ("offensive nickname", Mutate(good, pk => { pk.Nickname = "SHIT"; pk.IsNicknamed = true; }), "INAPPROPRIATE_NAME"),
};
foreach (var fixture in illegal)
    await Test(fixture.Name, () =>
    {
        var reply = Policy.Analyze(fixture.Data);
        CheckReply(reply, fixture.Data);
        Require(!reply.Legal && reply.ReasonCodes.Contains(fixture.Reason), "missing expected reason: " + fixture.Name);
        return Task.CompletedTask;
    });

// Gen V: synthetic Black 2/White 2 and Black/White trainers and generated
// encounters only, as for Gen IV.
var trainer5 = new SimpleTrainerInfo(GameVersion.B2) { OT = "VALIDATOR", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
var trainerBW = new SimpleTrainerInfo(GameVersion.B) { OT = "VALIDATOR", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
const PokemonFormat Pk5 = PokemonFormat.FormatPk5;
byte[] Canonical5(PK5 pk)
{
    pk.RefreshChecksum();
    return pk.Data[..Policy.PayloadSize].ToArray();
}
byte[] Mutate5(byte[] original, Action<PK5> change)
{
    var pk = new PK5(original.ToArray());
    change(pk);
    return Canonical5(pk);
}
T Generate5<T>(ushort species, SimpleTrainerInfo origin, GameVersion version, byte form = 0) where T : IEncounterable =>
    EncounterMovesetGenerator.GenerateEncounters(new PK5 { Species = species, Form = form }, origin,
        ReadOnlyMemory<ushort>.Empty, [version]).OfType<T>().First(e => !e.IsEgg && e.Form == form);
byte[] Convert5(IEncounterable encounter, SimpleTrainerInfo origin)
{
    var pk = (PK5)encounter.ConvertToPKM(origin);
    pk.MetDate = new DateOnly(2020, 1, 1);
    return Canonical5(pk);
}
// Patrat lives in both Unova games' personal tables and holds nothing.
var wild5 = Convert5(Generate5<EncounterSlot5>(504, trainer5, GameVersion.B2), trainer5);
ValidateReply ExpectLegal5(byte[] data, Destination destinations)
{
    var reply = Policy.Analyze(Pk5, data);
    CheckReply(reply, data, Pk5);
    Require(reply.Legal, "legal PK5 fixture rejected: " + string.Join(',', reply.ReasonCodes));
    Require(reply.Destinations == (uint)destinations, $"destinations {(Destination)reply.Destinations}, expected {destinations}");
    return reply;
}
// The game clears the nickname buffer before writing a new name; leftover bytes are illegal trash.
void Nickname5(PK5 pk, string name)
{
    pk.NicknameTrash.Clear();
    pk.Nickname = name;
    pk.IsNicknamed = true;
}
Destination DestinationsOf5(byte[] data, Action<PK5> change) => Policy.Destinations(new PK5(Mutate5(data, change)));

await Test("legal Gen V wild encounters fit Black/White and Black 2/White 2", () =>
{
    byte[] before = wild5.ToArray();
    ExpectLegal5(wild5, Policy.Gen5Destinations);
    Require(wild5.SequenceEqual(before), "input was changed");
    ExpectLegal5(Convert5(Generate5<EncounterSlot5>(509, trainerBW, GameVersion.B), trainerBW), Policy.Gen5Destinations);
    return Task.CompletedTask;
});

await Test("Black 2/White 2 forms fit only Black 2/White 2", () =>
{
    // Dream Radar Therian Tornadus: a Black 2/White 2-only form, from generated encounter data.
    var therian = Generate5<IEncounterable>(641, trainer5, GameVersion.B2, 1);
    ExpectLegal5(Convert5(therian, trainer5), Destination.B2W2);
    foreach (ushort species in new ushort[] { 641, 642, 645 })
        Require(DestinationsOf5(wild5, pk => { pk.Species = species; pk.Form = 1; }) == Destination.B2W2,
            $"species {species} Therian destinations");
    Require(DestinationsOf5(wild5, pk => { pk.Species = 641; pk.Form = 0; }) == Policy.Gen5Destinations, "Incarnate destinations");
    return Task.CompletedTask;
});

await Test("Gen V held items and balls follow both Unova games", () =>
{
    // Rocky Helmet (540) is a Gen V holdable item; Dream Ball (25) is Gen V's last ball.
    Require(DestinationsOf5(wild5, pk => pk.HeldItem = 540) == Policy.Gen5Destinations, "Gen V item destinations");
    Require(DestinationsOf5(wild5, pk => pk.HeldItem = 999) == Destination.None, "out-of-range item destinations");
    Require(DestinationsOf5(wild5, pk => pk.Ball = (byte)Ball.Dream) == Policy.Gen5Destinations, "Dream Ball destinations");
    Require(DestinationsOf5(wild5, pk => pk.Ball = (byte)Ball.Beast) == Destination.None, "later ball destinations");
    return Task.CompletedTask;
});

await Test("Gen IV and Gen V verdicts never cross pools", () =>
{
    // A legal PK4 never reports Gen V bits, a legal PK5 never Gen IV bits, and
    // a payload sent under the other format never reports the other pool.
    Require((Policy.Analyze(good).Destinations & (uint)Policy.Gen5Destinations) == 0, "PK4 reported Gen V");
    Require((Policy.Analyze(Pk5, wild5).Destinations & (uint)Policy.Gen4Destinations) == 0, "PK5 reported Gen IV");
    var crossed = Policy.Analyze(wild5);
    CheckReply(crossed, wild5);
    crossed = Policy.Analyze(Pk5, good);
    CheckReply(crossed, good, Pk5);
    Require(Policy.DestinationsFor(PokemonFormat.FormatPk4) == Policy.Gen4Destinations &&
        Policy.DestinationsFor(Pk5) == Policy.Gen5Destinations, "format destination masks");
    return Task.CompletedTask;
});

await Test("Gen V names follow the Switch word list", () =>
{
    void Name(byte[] data, bool allowed, string label)
    {
        var reply = Policy.Analyze(Pk5, data);
        CheckReply(reply, data, Pk5);
        Require(allowed ? reply.Legal : !reply.Legal && reply.ReasonCodes.SequenceEqual(["INAPPROPRIATE_NAME"]),
            label + ": " + string.Join(',', reply.ReasonCodes));
    }
    byte[] Nick(string name) => Mutate5(wild5, pk => Nickname5(pk, name));
    Name(Nick("SHIT"), false, "offensive Gen V nickname");
    Name(Nick("KILLER"), false, "blocked Gen V inflection");
    var named = new SimpleTrainerInfo(GameVersion.B2) { OT = "SHIT", TID16 = 12345, SID16 = 54321, Language = 2, Gender = 0 };
    Name(Convert5(Generate5<EncounterSlot5>(504, named, GameVersion.B2), named), false, "offensive Gen V trainer");
    // Allow-list entries and official terms, including Gen V names.
    foreach (var name in new[] { "LULU", "GRAPE", "LEAFAGE", "PATRAT", "Snivy", "VCREATE" })
        Name(Nick(name), true, name);
    return Task.CompletedTask;
});

await Test("N's Pokémon and untradable Gen V forms are refused", () =>
{
    // A generated N's Pokémon is legal to the core, but carries N's sparkle.
    var n = Convert5(Generate5<EncounterStatic5N>(509, trainer5, GameVersion.B2), trainer5);
    Require(new PK5(n.ToArray()).NSparkle, "N fixture lacks the sparkle");
    var reply = Policy.Analyze(Pk5, n);
    CheckReply(reply, n, Pk5);
    Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(["INCOMPATIBLE_FORM"]), "N's Pokémon accepted");
    // Fused Kyurem and battle-only Zen Mode exist in the personal table but cannot be traded.
    foreach (var (species, form) in new (ushort, byte)[] { (646, 1), (646, 2), (555, 1) })
        Require(DestinationsOf5(wild5, pk => { pk.Species = species; pk.Form = form; }) == Destination.None,
            $"species {species} form {form} destinations");
    Require(DestinationsOf5(wild5, pk => pk.Species = 646) == Policy.Gen5Destinations, "Kyurem destinations");
    return Task.CompletedTask;
});

var illegal5 = new (string Name, byte[] Data, string Reason)[]
{
    ("Gen V egg", Mutate5(wild5, pk => pk.IsEgg = true), "EGG_NOT_ALLOWED"),
    ("Gen V mail", Mutate5(wild5, pk => pk.HeldItem = 137), "MAIL_NOT_ALLOWED"),
    ("Gen V future species", Mutate5(wild5, pk => pk.Species = 650), "INVALID_SPECIES"),
    ("Gen V empty species", Mutate5(wild5, pk => pk.Species = 0), "INVALID_SPECIES"),
    ("Gen V fused Kyurem", Mutate5(wild5, pk => { pk.Species = 646; pk.Form = 1; }), "INCOMPATIBLE_FORM"),
    ("Gen V invalid form", Mutate5(wild5, pk => pk.Form = 31), "INCOMPATIBLE_FORM"),
    ("Gen V N's sparkle on a wild Pokémon", Mutate5(wild5, pk => pk.NSparkle = true), "INCOMPATIBLE_FORM"),
    ("Gen V future move", Mutate5(wild5, pk => pk.Move1 = 600), "ILLEGAL_MOVES"),
    ("Gen V invalid EV sum", Mutate5(wild5, pk => { pk.EV_HP = 255; pk.EV_ATK = 255; pk.EV_DEF = 255; }), "ILLEGAL_DATA"),
    // Gen V wild PIDs correlate bit 31 with the trainer ID.
    ("Gen V invalid PID correlation", Mutate5(wild5, pk => pk.PID ^= 0x80000000), "ILLEGAL_RNG"),
    ("Gen V offensive nickname", Mutate5(wild5, pk => Nickname5(pk, "SHIT")), "INAPPROPRIATE_NAME"),
};
foreach (var fixture in illegal5)
    await Test(fixture.Name, () =>
    {
        var reply = Policy.Analyze(Pk5, fixture.Data);
        CheckReply(reply, fixture.Data, Pk5);
        Require(!reply.Legal && reply.ReasonCodes.Contains(fixture.Reason),
            $"missing expected reason {fixture.Reason}: {string.Join(',', reply.ReasonCodes)}");
        return Task.CompletedTask;
    });

await Test("malformed Gen V structures fail without repair", () =>
{
    // Sanity, checksum, the encryption sentinel and every other unused byte.
    foreach (int offset in new[] { 4, 6, 0x43, 0x47, 0x5E, 0x63, 0x64, 0x67, 0x86 })
    {
        var malformed = wild5.ToArray();
        malformed[offset] ^= 1;
        if (offset > 7)
        {
            uint sum = 0;
            for (int i = 8; i < malformed.Length; i += 2) sum += BinaryPrimitives.ReadUInt16LittleEndian(malformed.AsSpan(i));
            BinaryPrimitives.WriteUInt16LittleEndian(malformed.AsSpan(6), (ushort)sum);
        }
        byte[] before = malformed.ToArray();
        var reply = Policy.Analyze(Pk5, malformed);
        Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(new[] { "INVALID_STRUCTURE" }), $"malformed PK5 at {offset} accepted");
        Require(malformed.SequenceEqual(before), "malformed PK5 was repaired");
    }
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
    Require(reply.Legal && reply.Destinations == (uint)Policy.Gen4Destinations, "worker rejected legal input");
    Console.WriteLine($"Worker cold validation: {clock.ElapsedMilliseconds} ms");
    reply = await runner.Run(illegal[0].Data, CancellationToken.None);
    Require(!reply.Legal, "worker accepted egg");
    // The slowest name path loads the allow-list and every game-term list the first time a worker uses it.
    var term = Nicknamed("LEAFAGE");
    clock.Restart();
    reply = await runner.Run(term, CancellationToken.None);
    Require(reply.Legal, "worker refused a Pokémon term");
    Console.WriteLine($"Worker first name exemption: {clock.ElapsedMilliseconds} ms");
    reply = await runner.Run(Nicknamed("SHIT"), CancellationToken.None);
    Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(["INAPPROPRIATE_NAME"]), "worker accepted an offensive name");
    // The same worker serves Gen V payloads with Gen V destinations.
    clock.Restart();
    reply = await runner.Run(Pk5, wild5, CancellationToken.None);
    CheckReply(reply, wild5, Pk5);
    Require(reply.Legal && reply.Destinations == (uint)Policy.Gen5Destinations, "worker rejected legal PK5");
    Console.WriteLine($"Worker first Gen V validation: {clock.ElapsedMilliseconds} ms");
    reply = await runner.Run(Pk5, illegal5[0].Data, CancellationToken.None);
    Require(!reply.Legal && reply.ReasonCodes.SequenceEqual(["EGG_NOT_ALLOWED"]), "worker accepted a Gen V egg");
    reply = await runner.Run(good, CancellationToken.None);
    Require(reply.Legal && reply.Destinations == (uint)Policy.Gen4Destinations, "worker changed a Gen IV verdict after Gen V");
});

await Test("worker requests lead with the format byte", async () =>
{
    foreach (var (format, data) in new[] { (PokemonFormat.FormatPk4, good), (Pk5, wild5) })
    {
        string path = Path.Combine(Path.GetTempPath(), "validator-test-" + Guid.NewGuid().ToString("N"));
        using var runner = new WorkerRunner(() =>
        {
            var start = Shell("head -c 137 > \"$1\"; exec sleep 20");
            start.ArgumentList.Add("fixture");
            start.ArgumentList.Add(path);
            return start;
        }, TimeSpan.FromMilliseconds(500));
        try
        {
            await ExpectStatus(async () => await runner.Run(format, data, CancellationToken.None), StatusCode.Unavailable);
            var frame = await File.ReadAllBytesAsync(path);
            Require(frame.Length == 137 && frame[0] == (byte)format && frame.AsSpan(1).SequenceEqual(data), "request frame");
        }
        finally { File.Delete(path); }
    }
    // Unknown formats never reach a worker.
    using var unused = new WorkerRunner(() => Shell("exit 2"), TimeSpan.FromMilliseconds(200));
    await ExpectStatus(async () => await unused.Run((PokemonFormat)2, good, CancellationToken.None), StatusCode.InvalidArgument);
});

await Test("a worker given an unknown format byte exits without replying", async () =>
{
    var start = new ProcessStartInfo("dotnet") { RedirectStandardInput = true, RedirectStandardOutput = true, RedirectStandardError = true };
    start.ArgumentList.Add(typeof(Policy).Assembly.Location);
    start.ArgumentList.Add("--worker");
    using var process = Process.Start(start)!;
    _ = process.StandardError.BaseStream.CopyToAsync(Stream.Null);
    var replies = new MemoryStream();
    var output = process.StandardOutput.BaseStream.CopyToAsync(replies);
    await process.StandardInput.BaseStream.WriteAsync(new byte[] { 2 }.Concat(good).ToArray());
    await process.StandardInput.BaseStream.FlushAsync();
    using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(30));
    await process.WaitForExitAsync(timeout.Token);
    await output;
    Require(process.ExitCode == 2 && replies.Length == 0, $"worker exit {process.ExitCode}, {replies.Length} reply bytes");
});

ProcessStartInfo Shell(string command)
{
    var start = new ProcessStartInfo("/bin/sh");
    start.ArgumentList.Add("-c");
    start.ArgumentList.Add(command);
    return start;
}
// A worker command that appends a line to a file each time a process starts.
ProcessStartInfo Counted(string path, string command)
{
    var start = Shell("echo started >> \"$1\"; " + command);
    start.ArgumentList.Add("fixture");
    start.ArgumentList.Add(path);
    start.ArgumentList.Add(typeof(Policy).Assembly.Location);
    return start;
}
int Starts(string path) => File.Exists(path) ? File.ReadAllLines(path).Length : 0;

await Test("a worker that answered serves later payloads until recycled", async () =>
{
    string path = Path.Combine(Path.GetTempPath(), "validator-test-" + Guid.NewGuid().ToString("N"));
    using var runner = new WorkerRunner(() => Counted(path, "exec dotnet \"$2\" --worker"), TimeSpan.FromSeconds(5), 1);
    try
    {
        var egg = illegal[0].Data;
        for (int i = 0; i < WorkerRunner.MaxUses; i++)
        {
            var data = i % 2 == 0 ? good : egg;
            var reply = await runner.Run(data, CancellationToken.None);
            CheckReply(reply, data);
            Require(reply.Legal == (i % 2 == 0), "reused worker changed a verdict");
        }
        Require(Starts(path) == 1, $"reused worker restarted: {Starts(path)} starts");
        var last = await runner.Run(good, CancellationToken.None);
        Require(last.Legal && Starts(path) == 2, "worker was not recycled after its last use");
    }
    finally { File.Delete(path); }
});

await Test("a worker that failed is never reused", async () =>
{
    string path = Path.Combine(Path.GetTempPath(), "validator-test-" + Guid.NewGuid().ToString("N"));
    using var runner = new WorkerRunner(() => Counted(path, "head -c 4096 /dev/zero; exec sleep 20"), TimeSpan.FromMilliseconds(500), 1);
    try
    {
        await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.Unavailable);
        await ExpectStatus(async () => await runner.Run(good, CancellationToken.None), StatusCode.Unavailable);
        Require(Starts(path) == 2, $"failed worker reused: {Starts(path)} starts");
    }
    finally { File.Delete(path); }
});

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
    ValidateRequest Request(byte[] bytes, PokemonFormat format = PokemonFormat.FormatPk4) =>
        new() { Pokemon = new() { Pk4 = ByteString.CopyFrom(bytes), Sha256 = ByteString.CopyFrom(SHA256.HashData(bytes)) }, Format = format };
    // Before any call, so the first worker's start-up outlasts the deadline; a warm worker can answer within 1 ms.
    await ExpectStatus(async () => await client.ValidateAsync(Request(good), deadline: DateTime.UtcNow.AddMilliseconds(1)), StatusCode.DeadlineExceeded);
    var reply = await client.ValidateAsync(Request(good), deadline: DateTime.UtcNow.AddSeconds(2));
    CheckReply(reply, good);
    Require(reply.Legal && reply.Destinations == (uint)Policy.Gen4Destinations, "gRPC rejected legal payload");
    reply = await client.ValidateAsync(Request(wild5, Pk5), deadline: DateTime.UtcNow.AddSeconds(2));
    CheckReply(reply, wild5, Pk5);
    Require(reply.Legal && reply.Destinations == (uint)Policy.Gen5Destinations, "gRPC rejected legal PK5 payload");
    await ExpectStatus(async () => await client.ValidateAsync(Request(good, (PokemonFormat)2)), StatusCode.InvalidArgument);
    var wrongHash = Request(good);
    wrongHash.Pokemon.Sha256 = ByteString.CopyFrom(new byte[32]);
    await ExpectStatus(async () => await client.ValidateAsync(wrongHash), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(new ValidateRequest()), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[135])), StatusCode.InvalidArgument);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[260])), StatusCode.ResourceExhausted);
    await ExpectStatus(async () => await client.ValidateAsync(Request(new byte[4096])), StatusCode.Unknown);
    await app.StopAsync();
});
Console.WriteLine($"{passed} validator scenarios passed.");
