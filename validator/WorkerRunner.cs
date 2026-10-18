using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using Grpc.Core;
using Platinumwt.V1;

namespace PlatinumWT.Validator;

// The core is synchronous and cannot be interrupted safely within a thread.
// Keep its lifetime, pipe buffers and admission bound inside this module.
public sealed class WorkerRunner : IDisposable
{
    private readonly SemaphoreSlim slots = new(2);
    private readonly Func<ProcessStartInfo> command;
    private readonly TimeSpan budget;

    public WorkerRunner() : this(Command, TimeSpan.FromSeconds(1)) { }
    public WorkerRunner(Func<ProcessStartInfo> command, TimeSpan budget)
    {
        this.command = command;
        this.budget = budget;
    }

    private static ProcessStartInfo Command()
    {
        var start = new ProcessStartInfo("dotnet");
        start.ArgumentList.Add(Assembly.GetExecutingAssembly().Location);
        start.ArgumentList.Add("--worker");
        return start;
    }

    public async Task<ValidateReply> Run(byte[] payload, CancellationToken cancellation)
    {
        if (!await slots.WaitAsync(0, cancellation))
            throw new RpcException(new Status(StatusCode.ResourceExhausted, "validator busy"));
        using var deadline = CancellationTokenSource.CreateLinkedTokenSource(cancellation);
        deadline.CancelAfter(budget);
        using var process = new Process();
        try
        {
            process.StartInfo = command();
            process.StartInfo.UseShellExecute = false;
            process.StartInfo.RedirectStandardInput = true;
            process.StartInfo.RedirectStandardOutput = true;
            process.StartInfo.RedirectStandardError = true;
            process.StartInfo.Environment["DOTNET_EnableDiagnostics"] = "0";
            process.StartInfo.Environment["DOTNET_PROCESSOR_COUNT"] = "2";
            process.StartInfo.Environment["DOTNET_GCHeapHardLimit"] = "8000000"; // 128 MiB/worker.
            if (!process.Start())
                throw new InvalidOperationException();
            var output = ReadBounded(process.StandardOutput.BaseStream, deadline.Token);
            var errors = process.StandardError.BaseStream.CopyToAsync(Stream.Null, deadline.Token);
            await process.StandardInput.BaseStream.WriteAsync(payload, deadline.Token);
            process.StandardInput.Close();
            await Task.WhenAll(process.WaitForExitAsync(deadline.Token), output, errors);
            if (process.ExitCode != 0)
                throw new InvalidOperationException();
            var reply = ValidateReply.Parser.ParseFrom(await output);
            if (reply.PolicyVersion != Policy.Version ||
                !reply.Sha256.Span.SequenceEqual(SHA256.HashData(payload)) ||
                reply.Legal != (reply.ReasonCodes.Count == 0) || reply.ReasonCodes.Count > 8 ||
                reply.ReasonCodes.Any(r => r.Length is 0 or > 32 || r.Any(c => c is not (>= 'A' and <= 'Z' or '_'))))
                throw new InvalidOperationException();
            return reply;
        }
        catch (Exception)
        {
            // Never serialize core exception messages, payloads or pipe output.
            throw new RpcException(new Status(cancellation.IsCancellationRequested ? StatusCode.Cancelled : StatusCode.Unavailable,
                "validation unavailable"));
        }
        finally
        {
            try
            {
                if (process.Id != 0 && !process.HasExited)
                    process.Kill(entireProcessTree: true);
                if (process.Id != 0)
                {
                    using var cleanup = new CancellationTokenSource(TimeSpan.FromSeconds(1));
                    await process.WaitForExitAsync(cleanup.Token);
                }
            }
            catch (InvalidOperationException) { }
            catch (OperationCanceledException) { }
            slots.Release();
        }
    }

    private static async Task<byte[]> ReadBounded(Stream stream, CancellationToken cancellation)
    {
        byte[] buffer = new byte[1025];
        int used = 0;
        while (used < buffer.Length)
        {
            int count = await stream.ReadAsync(buffer.AsMemory(used), cancellation);
            if (count == 0)
                return buffer[..used];
            used += count;
        }
        throw new InvalidOperationException("worker output limit");
    }

    public void Dispose() => slots.Dispose();
}
