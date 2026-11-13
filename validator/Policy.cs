using System.Buffers.Binary;
using System.Security.Cryptography;
using Google.Protobuf;
using PKHeX.Core;
using Dswt.V1;

namespace DSWT.Validator;

// Bits of ValidateReply.destinations: the Gen IV games that can hold the Pokémon.
[Flags]
public enum Destination : uint
{
    None = 0,
    DP = 1,
    Pt = 2,
    HGSS = 4,
}

public static class Policy
{
    // Bump for changes to the adapter, core, resources or parsing settings.
    public const string Version = "pkhex-26.08.26-74b88906-dswt3";
    public const int PayloadSize = 136;
    public const Destination AllDestinations = Destination.DP | Destination.Pt | Destination.HGSS;

    public static ValidateReply Analyze(byte[] data)
    {
        if (data.Length != PayloadSize)
            throw new ArgumentException("payload length");
        var reply = new ValidateReply { PolicyVersion = Version, Sha256 = ByteString.CopyFrom(SHA256.HashData(data)) };
        ValidateReply Reject(string reason)
        {
            reply.ReasonCodes.Add(reason);
            return reply;
        }

        uint sum = 0;
        for (int offset = 8; offset < PayloadSize; offset += 2)
            sum += BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(offset));
        if (BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(4)) != 0 ||
            BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(6)) != (ushort)sum)
            return Reject("INVALID_STRUCTURE");
        // PK4's constructor guesses encrypted input from this reserved field.
        // Require canonical input first so that guess is never taken.
        foreach (int offset in new[] { 0x42, 0x43, 0x5E, 0x63, 0x64, 0x65, 0x66, 0x67 })
            if (data[offset] != 0)
                return Reject("INVALID_STRUCTURE");
        ushort species = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(8));
        if (species is 0 or > 493)
            return Reject("INVALID_SPECIES");

        var pk = new PK4(data.ToArray());
        if (pk.IsEgg)
            return Reject("EGG_NOT_ALLOWED");
        if (ItemStorage4.Mail.Contains((ushort)pk.HeldItem))
            return Reject("MAIL_NOT_ALLOWED");
        // A species/form no Gen IV game can hold or trade is rejected before analysis.
        if (IsUntradable(pk) || !Games.Any(game => FormFits(game.Save, pk)))
            return Reject("INCOMPATIBLE_FORM");
        var destinations = Destinations(pk);

        ParseSettings.Initialize(new LegalitySettings());
        // Gen IV names predate the later consoles' word lists.
        ParseSettings.Settings.WordFilter.DisableWordFilterPastGen = true;
        // No active save: legality must not assume any one Gen IV game is the
        // Pokémon's home. Destinations are reported separately instead.
        ParseSettings.ClearActiveTrainer();
        ParseSettings.AllowEraCartGBA = true;
        ParseSettings.AllowEraSwitchGBA = false;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound3 = Severity.Invalid;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound4 = Severity.Invalid;
        var analysis = new LegalityAnalysis(pk);
        if (!analysis.Parsed || !pk.Data[..PayloadSize].SequenceEqual(data))
            throw new InvalidOperationException("analysis unavailable");
        if (analysis.Valid)
        {
            // Legal, but no DP, Pt or HGSS save can hold it.
            if (destinations == Destination.None)
                return Reject("INCOMPATIBLE_GAME");
            reply.Legal = true;
            reply.Destinations = (uint)destinations;
            return reply;
        }

        var reasons = new SortedSet<string>(StringComparer.Ordinal);
        foreach (var result in analysis.Results.Where(r => !r.Valid))
            reasons.Add(result.Identifier switch
            {
                CheckIdentifier.Encounter or CheckIdentifier.Fateful or CheckIdentifier.GameOrigin => "ILLEGAL_ENCOUNTER",
                CheckIdentifier.PID or CheckIdentifier.EC or CheckIdentifier.Shiny or CheckIdentifier.IVs => "ILLEGAL_RNG",
                CheckIdentifier.CurrentMove or CheckIdentifier.RelearnMove => "ILLEGAL_MOVES",
                CheckIdentifier.Form => "INCOMPATIBLE_FORM",
                _ => "ILLEGAL_DATA",
            });
        if (analysis.Info.Moves.Any(m => !m.Valid) || analysis.Info.Relearn.Any(m => !m.Valid))
            reasons.Add("ILLEGAL_MOVES");
        if (reasons.Count == 0)
            reasons.Add("ILLEGAL_DATA");
        reply.ReasonCodes.AddRange(reasons);
        return reply;
    }

    // Blank saves expose the pinned core's per-game limits (Personal, HeldItems,
    // MaxItemID, MaxBallID), the same members SaveExtensions.EvaluateCompatibility reads.
    private static readonly (Destination Game, SaveFile Save)[] Games =
        [(Destination.DP, new SAV4DP()), (Destination.Pt, new SAV4Pt()), (Destination.HGSS, new SAV4HGSS())];

    // Which of DP, Pt and HGSS can hold this Pokémon; legality is checked separately.
    public static Destination Destinations(PK4 pk)
    {
        if (IsUntradable(pk))
            return Destination.None;
        var result = Destination.None;
        foreach (var (game, save) in Games)
            if (FormFits(save, pk) && ItemFits(save, pk) && BallFits(game, save, pk))
                result |= game;
        return result;
    }

    // TradeRestrictions.IsUntradable: Spiky-eared Pichu (172, form 1) cannot be traded
    // from any game. PersonalTable4.IsPresentInGame accepts that form in every table.
    private static bool IsUntradable(PK4 pk) => TradeRestrictions.IsUntradable(pk.Species, pk.Form, 0, 4);

    // PersonalTable.DP/Pt/HGSS.IsPresentInGame: the species and form exist in that
    // game's personal table, so Platinum forms (Giratina Origin, Shaymin Sky, Rotom
    // appliances) fit only the games whose table lists them.
    private static bool FormFits(SaveFile save, PK4 pk) => save.Personal.IsPresentInGame(pk.Species, pk.Form);

    // SaveExtensions.GetSaveFileErrata: the held item is within MaxItemID
    // (Legal.MaxItemID_4_DP/Pt/HGSS) and in HeldItems (Legal.HeldItems_DP/Pt/HGSS;
    // the pinned core defines HGSS's holdable items as Platinum's).
    private static bool ItemFits(SaveFile save, PK4 pk)
    {
        ushort item = (ushort)pk.HeldItem;
        return item == 0 || (item <= save.MaxItemID && save.HeldItems.Contains(item));
    }

    // PK4 splits the ball. D/P/Pt read BallDPPt (0x83), at most Cherish
    // (MiscVerifierG4.IsValidBallDP; pokeplatinum PokemonDataBlockD.pokeball, and
    // Platinum leaves the HGSS byte at 0x86 unused). HG/SS show the higher of 0x86 and
    // 0x83 (G4PKM.Ball), at most Sport (Legal.MaxBallID_4 via SAV4.MaxBallID). So an
    // Apricorn Ball shows as its 0x83 Poké Ball in D/P/Pt.
    private static bool BallFits(Destination game, SaveFile save, PK4 pk) => game == Destination.HGSS
        ? pk.Ball <= save.MaxBallID
        : pk.BallDPPt <= (byte)Ball.Cherish;
}
