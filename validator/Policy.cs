using System.Buffers.Binary;
using System.Security.Cryptography;
using Google.Protobuf;
using PKHeX.Core;
using Dswt.V1;

namespace DSWT.Validator;

// Bits of ValidateReply.destinations: the games that can hold the Pokémon.
// Gen IV and Gen V are separate pools; a reply carries only its format's bits.
[Flags]
public enum Destination : uint
{
    None = 0,
    DP = 1,
    Pt = 2,
    HGSS = 4,
    BW = 8,
    B2W2 = 16,
}

public static class Policy
{
    // Bump for changes to the adapter, core, resources or parsing settings.
    public const string Version = "pkhex-26.08.26-74b88906-dswt5";
    // PK4 and PK5 stored structures are both 136 bytes.
    public const int PayloadSize = 136;
    public const Destination Gen4Destinations = Destination.DP | Destination.Pt | Destination.HGSS;
    public const Destination Gen5Destinations = Destination.BW | Destination.B2W2;

    public static bool IsKnown(PokemonFormat format) => format is PokemonFormat.FormatPk4 or PokemonFormat.FormatPk5;

    // The only bits a reply for this format may set.
    public static Destination DestinationsFor(PokemonFormat format) => format switch
    {
        PokemonFormat.FormatPk4 => Gen4Destinations,
        PokemonFormat.FormatPk5 => Gen5Destinations,
        _ => throw new ArgumentOutOfRangeException(nameof(format)),
    };

    public static ValidateReply Analyze(byte[] data) => Analyze(PokemonFormat.FormatPk4, data);

    public static ValidateReply Analyze(PokemonFormat format, byte[] data)
    {
        if (data.Length != PayloadSize)
            throw new ArgumentException("payload length");
        if (!IsKnown(format))
            throw new ArgumentOutOfRangeException(nameof(format));
        bool gen5 = format == PokemonFormat.FormatPk5;
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
        // The PK4 and PK5 constructors guess encrypted input from the reserved
        // field at 0x64. Require canonical input first so that guess is never taken.
        foreach (int offset in gen5 ? ReservedPK5 : ReservedPK4)
            if (data[offset] != 0)
                return Reject("INVALID_STRUCTURE");
        ushort species = BinaryPrimitives.ReadUInt16LittleEndian(data.AsSpan(8));
        if (species == 0 || species > (gen5 ? (int)Species.Genesect : (int)Species.Arceus))
            return Reject("INVALID_SPECIES");

        PKM pk = gen5 ? new PK5(data.ToArray()) : new PK4(data.ToArray());
        if (pk.IsEgg)
            return Reject("EGG_NOT_ALLOWED");
        // Gen V kept Gen IV's mail item IDs, Greet Mail (137) to BridgeMail M (148).
        if (ItemStorage4.Mail.Contains((ushort)pk.HeldItem))
            return Reject("MAIL_NOT_ALLOWED");
        // A species/form no game of the format can hold or trade is rejected before analysis.
        if (IsUntradable(pk) || !GamesFor(format).Any(game => FormFits(game.Save, pk)))
            return Reject("INCOMPATIBLE_FORM");
        var destinations = Destinations(pk);

        ParseSettings.Initialize(new LegalitySettings());
        // Names are a content rule checked below, never legality: Gen IV names predate
        // the later consoles' word lists, and the core would otherwise apply Gen V's own
        // list as legality. Gen IV analysis already skipped the word filter.
        ParseSettings.Settings.WordFilter.DisableWordFilterPastGen = true;
        ParseSettings.Settings.WordFilter.CheckWordFilter = false;
        // No active save: legality must not assume any one game is the
        // Pokémon's home. Destinations are reported separately instead.
        ParseSettings.ClearActiveTrainer();
        ParseSettings.AllowEraCartGBA = true;
        ParseSettings.AllowEraSwitchGBA = false;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound3 = Severity.Invalid;
        ParseSettings.Settings.FramePattern.RNGFrameNotFound4 = Severity.Invalid;
        var analysis = new LegalityAnalysis(pk);
        if (!analysis.Parsed || !pk.Data[..PayloadSize].SequenceEqual(data))
            throw new InvalidOperationException("analysis unavailable");
        // Encounters the core marks untradable, such as a starter that must stay home.
        if (TradeRestrictions.IsUntradableEncounter(analysis.EncounterOriginal))
            return Reject("INCOMPATIBLE_FORM");
        // Another player sees these names, so they follow the newest games' online rules.
        var inappropriate = HasInappropriateName(pk);
        if (analysis.Valid && inappropriate)
            return Reject("INAPPROPRIATE_NAME");
        if (analysis.Valid)
        {
            // Legal, but no save of its format (DP, Pt, HGSS or BW, B2W2) can hold it.
            if (destinations == Destination.None)
                return Reject("INCOMPATIBLE_GAME");
            reply.Legal = true;
            reply.Destinations = (uint)destinations;
            return reply;
        }

        var reasons = new SortedSet<string>(StringComparer.Ordinal);
        if (inappropriate)
            reasons.Add("INAPPROPRIATE_NAME");
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

    // The Nintendo Switch word list (WordFilterNX), the newest games' online name rules,
    // checks the nickname when the player gave one and the original trainer's name.
    // ParseSettings keeps it out of legality: Gen IV and V names are not evidence
    // of an illegal Pokémon, so a match is its own reason rather than ILLEGAL_DATA.
    public static bool HasInappropriateName(PKM pk) =>
        (pk.IsNicknamed && IsInappropriate(pk.Nickname, pk.Context)) || IsInappropriate(pk.OriginalTrainerName, pk.Context);

    private static bool IsInappropriate(string name, EntityContext context)
    {
        if (!WordFilterNX.IsFiltered(name, out _, context))
            return false;
        var key = Key(name);
        return !AllowedNames.Value.Contains(key) && !GameTerms.Value.Contains(key);
    }

    // The word list's own normalization: no spaces, lowercase, folded kana and widths.
    private static string Key(string name)
    {
        Span<char> buffer = stackalloc char[name.Length];
        return new string(buffer[..TextNormalizer.Normalize(name, buffer)]);
    }

    // Reviewed names the list's substring patterns catch by accident (name-allowlist.txt).
    private static readonly Lazy<HashSet<string>> AllowedNames = new(LoadAllowedNames);

    private static HashSet<string> LoadAllowedNames()
    {
        using var stream = typeof(Policy).Assembly.GetManifestResourceStream("DSWT.Validator.name-allowlist.txt")
            ?? throw new InvalidOperationException("name allow-list missing");
        using var reader = new StreamReader(stream);
        var names = new HashSet<string>(StringComparer.Ordinal);
        while (reader.ReadLine() is { } line)
            if (line.Length != 0 && line[0] != '#')
                names.Add(Key(line));
        return names;
    }

    // Official Pokémon names in every Gen IV and Gen V language (the same seven):
    // species, moves, items, abilities, natures and forms, such as Leafage,
    // Coaching or Snatch. The pinned core's lists are its newest, so they hold
    // every Gen IV and Gen V term.
    private static readonly Lazy<HashSet<string>> GameTerms = new(() =>
    {
        var terms = new HashSet<string>(StringComparer.Ordinal);
        // Only the six lists, not all of GameStrings: the worker has a small heap.
        foreach (var language in new[] { "ja", "en", "fr", "it", "de", "es", "ko" })
            foreach (var list in new[] { "species", "moves", "items", "abilities", "natures", "forms" })
                foreach (var term in Util.GetStringList(list, language))
                    if (!string.IsNullOrWhiteSpace(term))
                        terms.Add(Key(term));
        return terms;
    });

    // PK4: 0x42-0x43, 0x5E and 0x63-0x67 are unused (PK4.ExtraBytes, less 0x86,
    // which holds the HG/SS ball). PK5: PK5.ExtraBytes, every byte the structure leaves unused.
    private static readonly int[] ReservedPK4 = [0x42, 0x43, 0x5E, 0x63, 0x64, 0x65, 0x66, 0x67];
    private static readonly int[] ReservedPK5 = [0x43, 0x44, 0x45, 0x46, 0x47, 0x5E, 0x63, 0x64, 0x65, 0x66, 0x67, 0x86];

    // Blank saves expose the pinned core's per-game limits (Personal, HeldItems,
    // MaxItemID, MaxBallID), the same members SaveExtensions.EvaluateCompatibility reads.
    // Each format's saves are built on first use, so a worker that never sees one format
    // never pays for its saves.
    private static readonly Lazy<(Destination Game, SaveFile Save)[]> Games4 = new(() =>
        [(Destination.DP, new SAV4DP()), (Destination.Pt, new SAV4Pt()), (Destination.HGSS, new SAV4HGSS())]);
    private static readonly Lazy<(Destination Game, SaveFile Save)[]> Games5 = new(() =>
        [(Destination.BW, new SAV5BW()), (Destination.B2W2, new SAV5B2W2())]);

    private static (Destination Game, SaveFile Save)[] GamesFor(PokemonFormat format) =>
        format == PokemonFormat.FormatPk5 ? Games5.Value : Games4.Value;

    // Which games of the Pokémon's own format can hold it: DP, Pt and HGSS for a PK4,
    // BW and B2W2 for a PK5. Legality is checked separately.
    public static Destination Destinations(PKM pk)
    {
        if (IsUntradable(pk))
            return Destination.None;
        var result = Destination.None;
        foreach (var (game, save) in GamesFor(pk is PK5 ? PokemonFormat.FormatPk5 : PokemonFormat.FormatPk4))
            if (FormFits(save, pk) && ItemFits(save, pk) && BallFits(game, save, pk))
                result |= game;
        return result;
    }

    // TradeRestrictions.IsUntradable: Spiky-eared Pichu (172, form 1) cannot be traded
    // from any game; in Gen V neither can fused Kyurem (646, forms 1-2) nor a
    // battle-only form. PersonalTable.IsPresentInGame accepts those forms.
    // PK5.NSparkle marks N's Pokémon, which DSWT does not trade.
    private static bool IsUntradable(PKM pk) =>
        TradeRestrictions.IsUntradable(pk.Species, pk.Form, 0, pk.Format) ||
        TradeRestrictions.IsUntradableHeld(pk.Context, pk.HeldItem) ||
        pk is PK5 { NSparkle: true };

    // PersonalTable.DP/Pt/HGSS/BW/B2W2.IsPresentInGame: the species and form exist in
    // that game's personal table, so Platinum forms (Giratina Origin, Shaymin Sky, Rotom
    // appliances) fit only the games whose table lists them, and Black 2/White 2 forms
    // (Therian Formes) fit only Black 2/White 2.
    private static bool FormFits(SaveFile save, PKM pk) => save.Personal.IsPresentInGame(pk.Species, pk.Form);

    // SaveExtensions.GetSaveFileErrata: the held item is within MaxItemID
    // (Legal.MaxItemID_4_DP/Pt/HGSS, MaxItemID_5_BW/B2W2) and in HeldItems
    // (Legal.HeldItems_DP/Pt/HGSS; the pinned core defines HGSS's holdable items as
    // Platinum's, and both Gen V games' as Legal.HeldItems_BW).
    private static bool ItemFits(SaveFile save, PKM pk)
    {
        ushort item = (ushort)pk.HeldItem;
        return item == 0 || (item <= save.MaxItemID && save.HeldItems.Contains(item));
    }

    // PK4 splits the ball. D/P/Pt read BallDPPt (0x83), at most Cherish
    // (MiscVerifierG4.IsValidBallDP; pokeplatinum PokemonDataBlockD.pokeball, and
    // Platinum leaves the HGSS byte at 0x86 unused). HG/SS show the higher of 0x86 and
    // 0x83 (G4PKM.Ball), at most Sport (Legal.MaxBallID_4 via SAV4.MaxBallID). So an
    // Apricorn Ball shows as its 0x83 Poké Ball in D/P/Pt. PK5 has the one ball byte
    // (0x83), at most Dream (Legal.MaxBallID_5 via SAV5.MaxBallID) in both Gen V games.
    private static bool BallFits(Destination game, SaveFile save, PKM pk) => pk switch
    {
        PK4 pk4 when game == Destination.HGSS => pk4.Ball <= save.MaxBallID,
        PK4 pk4 => pk4.BallDPPt <= (byte)Ball.Cherish,
        _ => pk.Ball <= save.MaxBallID,
    };
}
