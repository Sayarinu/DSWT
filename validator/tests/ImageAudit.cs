using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

// Test-only inspection of the exported image, never the test process's own core.
static class ImageAudit
{
    public static void Run(string root)
    {
        var source = Path.Combine(root, "usr/share/platinumwt-validator/source/validator");
        var resources = Path.Combine(source, ".deps/pkhex/PKHeX.Core/Resources");
        var assemblyPath = Path.GetFullPath(Path.Combine(root, "app/PKHeX.Core.dll"));
        var assembly = Assembly.LoadFile(assemblyPath);
        var names = assembly.GetManifestResourceNames().ToHashSet(StringComparer.Ordinal);
        var inventory = new SortedDictionary<string, object>(StringComparer.Ordinal);
        using var policy = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(source, "core-resources.json")));
        foreach (var category in policy.RootElement.GetProperty("resource_categories").EnumerateArray())
        {
            var directory = category.GetProperty("directory").GetString()!;
            var records = new StringBuilder();
            long total = 0;
            int count = 0;
            foreach (var path in Directory.GetFiles(Path.Combine(resources, directory), "*", SearchOption.AllDirectories)
                .OrderBy(p => Path.GetRelativePath(Path.Combine(resources, directory), p), StringComparer.Ordinal))
            {
                var relative = Path.GetRelativePath(Path.Combine(resources, directory), path).Replace('\\', '/');
                var name = "PKHeX.Core.Resources." + directory + "." + relative.Replace('/', '.');
                if (!names.Remove(name)) throw new InvalidDataException("Missing or ambiguous embedded resource: " + name);
                using var stream = assembly.GetManifestResourceStream(name)!;
                var digest = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
                var data = File.ReadAllBytes(path);
                if (digest != Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant())
                    throw new InvalidDataException("Assembly/source resource mismatch: " + name);
                inventory[name] = new { sha256 = digest, size = data.Length };
                records.Append(relative).Append('\0').Append(digest).Append('\n');
                total += data.Length;
                count++;
            }
            var categoryHash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(records.ToString()))).ToLowerInvariant();
            if (count != category.GetProperty("files").GetInt32() || total != category.GetProperty("bytes").GetInt64()
                || categoryHash != category.GetProperty("inventory_sha256").GetString())
                throw new InvalidDataException("Pinned resource inventory mismatch: " + directory);
        }
        if (names.Count != 0 || inventory.Count != 1731)
            throw new InvalidDataException("Unexpected embedded resource set");
        Console.WriteLine(JsonSerializer.Serialize(new {
            assembly_sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(assemblyPath))).ToLowerInvariant(),
            resources = inventory, count = inventory.Count,
        }));
    }
}
