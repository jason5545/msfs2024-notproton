// tlsservertest: does Wine's schannel accept a .NET server certificate as a TLS server?
//
// Makes its own self-signed RSA certificate, loads it several ways (each puts the private key in a
// different place: CNG/NCrypt ephemeral, CNG persisted, CAPI), then for each one runs an SslStream server
// on 127.0.0.1 and connects to it with an SslStream client in the same process.
// One line per case: "PASS <case>" or "FAIL <case> <exception chain>", then "SUMMARY pass=N fail=M".
// Runs anywhere (Windows, Wine); it touches only the current user's key stores for the persisted cases
// and removes those keys again.

using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;

static class Program
{
    static int pass, fail;

    static async Task<int> Main()
    {
        Console.WriteLine($"OS {Environment.OSVersion} 64bit={Environment.Is64BitProcess} .NET {Environment.Version}");

        using var rsa = RSA.Create(2048);
        var req = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        req.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
            new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));
        var san = new SubjectAlternativeNameBuilder();
        san.AddDnsName("localhost");
        req.CertificateExtensions.Add(san.Build());
        using var made = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddDays(30));
        // Exporting can fail under Wine; the serve-from-file case does not need it.
        byte[] pfx = Array.Empty<byte>();
        try { pfx = made.Export(X509ContentType.Pfx, "test"); }
        catch (Exception e) { Console.WriteLine($"NOTE pfx export failed: {e.GetType().Name}: {e.Message} (0x{e.HResult:x8})"); }

        // envcheck: GetUserNameW and a CAPI verify context in this process; "envcheck spawn" runs that in a
        // child started with an empty environment block (as some launchers start helpers) and with ours.
        {
            string[] a0 = Environment.GetCommandLineArgs().Skip(1).ToArray();
            if (a0.Length >= 1 && a0[0] == "envcheck")
            {
                if (a0.Length >= 2 && a0[1] == "spawn")
                {
                    foreach (bool empty in new[] { false, true })
                    {
                        var psi = new System.Diagnostics.ProcessStartInfo(Environment.ProcessPath!, "envcheck") { UseShellExecute = false, RedirectStandardOutput = true };
                        if (empty) { psi.Environment.Clear(); psi.Environment["SystemRoot"] = @"C:\windows"; }
                        using var p = System.Diagnostics.Process.Start(psi)!;
                        string o = p.StandardOutput.ReadToEnd().Trim(); p.WaitForExit();
                        Console.WriteLine($"CHILD env={(empty ? "empty" : "inherited")} {o}");
                    }
                    return 0;
                }
                var sb = new System.Text.StringBuilder(256); uint len = 256;
                bool okName = Native.GetUserNameW(sb, ref len);
                int errName = System.Runtime.InteropServices.Marshal.GetLastWin32Error();
                bool okProv = Native.CryptAcquireContextW(out IntPtr prov, null, null, 1, 0xF0000000);
                int errProv = System.Runtime.InteropServices.Marshal.GetLastWin32Error();
                if (okProv) Native.CryptReleaseContext(prov, 0);
                Console.WriteLine($"GetUserNameW={okName}({(okName ? sb.ToString() : "err " + errName)}) CryptAcquireContext(VERIFYCONTEXT)={okProv}{(okProv ? "" : $"(err 0x{errProv:x8})")} WINEUSERNAME={Environment.GetEnvironmentVariable("WINEUSERNAME") ?? "(unset)"}");
                return 0;
            }
        }

        // get <host> <path> [ip]: HTTPS GET to <host> through 127.0.0.1:443 (or ip), with the normal
        // certificate checks of the system (Wine's crypt32 chain + ROOT store) on <host>.
        {
            string[] a1 = Environment.GetCommandLineArgs().Skip(1).ToArray();
            if (a1.Length >= 3 && a1[0] == "get")
            {
                string ipArg = a1.Length >= 4 ? a1[3] : "127.0.0.1:443";
                string ip = ipArg.Split(':')[0]; int port = ipArg.Contains(':') ? int.Parse(ipArg.Split(':')[1]) : 443;
                var handler = new SocketsHttpHandler
                {
                    ConnectCallback = async (ctx, ct) =>
                    {
                        var sock = new Socket(SocketType.Stream, ProtocolType.Tcp);
                        await sock.ConnectAsync(IPAddress.Parse(ip), port, ct);
                        return new NetworkStream(sock, true);
                    },
                };
                SslPolicyErrors seen = SslPolicyErrors.None; string chainInfo = "";
                handler.SslOptions.RemoteCertificateValidationCallback = (_, c, chain, errors) =>
                {
                    seen = errors;
                    chainInfo = chain == null ? "" : string.Join(" <- ", chain.ChainElements.Select(e => e.Certificate.GetNameInfo(X509NameType.SimpleName, false)));
                    return errors == SslPolicyErrors.None;
                };
                using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(30) };
                try
                {
                    var r = await http.GetAsync($"https://{a1[1]}{a1[2]}");
                    var bytes = await r.Content.ReadAsByteArrayAsync();
                    Console.WriteLine($"GET {(int)r.StatusCode} {bytes.Length}B policy={seen} chain=[{chainInfo}]");
                    return 0;
                }
                catch (Exception e)
                {
                    var parts = new List<string>();
                    for (var x = e; x != null; x = x.InnerException) parts.Add($"{x.GetType().Name}: {x.Message}");
                    Console.WriteLine($"GET FAIL policy={seen} chain=[{chainInfo}] {string.Join(" <- ", parts)}");
                    return 1;
                }
            }
        }

        // serve <port> <case>: only the server side, for a client outside Wine (curl on the Mac).
        // Answers each connection with a small HTTP response; one line per connection.
        string[] args = Environment.GetCommandLineArgs().Skip(1).ToArray();
        if (args.Length >= 3 && args[0] == "serve")
        {
            X509Certificate2 cert = args[2] switch
            {
                "pfx-default" => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.DefaultKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
                "pfx-ephemeral" => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.EphemeralKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
                "capi-rsa" => WithCapiKey(made, rsa),
                var f when f.StartsWith("filee=") => X509CertificateLoader.LoadPkcs12FromFile(f[6..].Split('|')[0], f[6..].Split('|')[1], X509KeyStorageFlags.EphemeralKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
                var f when f.StartsWith("filem=") => X509CertificateLoader.LoadPkcs12FromFile(f[6..].Split('|')[0], f[6..].Split('|')[1], X509KeyStorageFlags.MachineKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
                var f when f.StartsWith("filemp=") => X509CertificateLoader.LoadPkcs12FromFile(f[7..].Split('|')[0], f[7..].Split('|')[1], X509KeyStorageFlags.MachineKeySet | X509KeyStorageFlags.PersistKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
#pragma warning disable SYSLIB0057
                var f when f.StartsWith("filector=") => new X509Certificate2(f[9..].Split('|')[0], f[9..].Split('|')[1]),
#pragma warning restore SYSLIB0057
                var f when f.StartsWith("file=") => X509CertificateLoader.LoadPkcs12FromFile(f[5..].Split('|')[0], f[5..].Split('|')[1], X509KeyStorageFlags.DefaultKeySet, Pkcs12LoaderLimits.DangerousNoLimits),
                _ => made,
            };
            var l = new TcpListener(IPAddress.Loopback, int.Parse(args[1]));
            l.Start();
            Console.WriteLine($"LISTEN {args[1]} {args[2]}");
            for (int i = 0; i < 3; i++)
            {
                using var tcp = await l.AcceptTcpClientAsync();
                try
                {
                    using var ssl = new SslStream(tcp.GetStream(), false);
                    var opts = new SslServerAuthenticationOptions { ServerCertificate = cert };
                    if (args.Length >= 4 && args[3] == "alpn")
                        opts.ApplicationProtocols = new List<SslApplicationProtocol> { SslApplicationProtocol.Http2, SslApplicationProtocol.Http11 };
                    await ssl.AuthenticateAsServerAsync(opts);
                    var buf = new byte[4096];
                    int n = await ssl.ReadAsync(buf);
                    await ssl.WriteAsync("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok"u8.ToArray());
                    await ssl.FlushAsync();
                    Console.WriteLine($"PASS conn {i} {ssl.SslProtocol} alpn={ssl.NegotiatedApplicationProtocol} read={n}");
                }
                catch (Exception e)
                {
                    var parts = new List<string>();
                    for (var x = e; x != null; x = x.InnerException)
                        parts.Add($"{x.GetType().Name}: {x.Message} (0x{x.HResult:x8})");
                    Console.WriteLine($"FAIL conn {i} {string.Join(" <- ", parts)}");
                }
            }
            return 0;
        }

        await Run("created-in-memory", () => made);
        await Run("pfx-default", () => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.DefaultKeySet, Pkcs12LoaderLimits.DangerousNoLimits));
        await Run("pfx-ephemeral", () => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.EphemeralKeySet, Pkcs12LoaderLimits.DangerousNoLimits));
        await Run("pfx-userkeyset", () => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.UserKeySet, Pkcs12LoaderLimits.DangerousNoLimits));
        await Run("pfx-exportable", () => X509CertificateLoader.LoadPkcs12(pfx, "test", X509KeyStorageFlags.Exportable, Pkcs12LoaderLimits.DangerousNoLimits));
        await Run("capi-rsa", () => WithCapiKey(made, rsa));

        Console.WriteLine($"SUMMARY pass={pass} fail={fail}");
        return fail == 0 ? 0 : 1;
    }

    // The same certificate with its key in a CAPI container (RSACryptoServiceProvider), the way
    // older code and many installers store it.
    static X509Certificate2 WithCapiKey(X509Certificate2 cert, RSA source)
    {
        var parms = new CspParameters(24) { KeyContainerName = "tlsservertest-" + Guid.NewGuid().ToString("N") };
        var capi = new RSACryptoServiceProvider(parms) { PersistKeyInCsp = false };
        capi.ImportParameters(source.ExportParameters(true));
        using var pub = X509CertificateLoader.LoadCertificate(cert.RawData);
        return pub.CopyWithPrivateKey(capi);
    }

    static async Task Run(string name, Func<X509Certificate2> load)
    {
        X509Certificate2? cert = null;
        try
        {
            cert = load();
            string keyInfo = Describe(cert);
            var listener = new TcpListener(IPAddress.Loopback, 0);
            listener.Start();
            int port = ((IPEndPoint)listener.LocalEndpoint).Port;

            var serverTask = Task.Run(async () =>
            {
                using var tcp = await listener.AcceptTcpClientAsync();
                using var ssl = new SslStream(tcp.GetStream(), false);
                await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
                {
                    ServerCertificate = cert,
                    EnabledSslProtocols = SslProtocols.Tls12,
                    ClientCertificateRequired = false,
                });
                var buf = new byte[4];
                int n = await ssl.ReadAsync(buf);
                await ssl.WriteAsync(buf.AsMemory(0, n));
                return ssl.SslProtocol + " " + ssl.NegotiatedCipherSuite;
            });

            using var client = new TcpClient();
            await client.ConnectAsync(IPAddress.Loopback, port);
            using var cssl = new SslStream(client.GetStream(), false, (_, _, _, _) => true);
            var clientTask = cssl.AuthenticateAsClientAsync(new SslClientAuthenticationOptions
            {
                TargetHost = "localhost",
                EnabledSslProtocols = SslProtocols.Tls12,
            });
            await Task.WhenAny(Task.WhenAll(serverTask, clientTask), Task.Delay(20000));
            // The server side is the one under test: report its error first.
            if (serverTask.IsFaulted) throw serverTask.Exception!.GetBaseException();
            if (!serverTask.IsCompleted || !clientTask.IsCompleted)
                throw new TimeoutException("handshake did not finish in 20 s");
            if (clientTask.IsFaulted) throw clientTask.Exception!.GetBaseException();
            await cssl.WriteAsync("ping"u8.ToArray());
            var echo = new byte[4];
            int got = await cssl.ReadAsync(echo);
            listener.Stop();
            string server = await serverTask;
            if (got != 4) throw new IOException($"echo returned {got} bytes");
            pass++;
            Console.WriteLine($"PASS {name} [{keyInfo}] {server}");
        }
        catch (Exception e)
        {
            fail++;
            var parts = new List<string>();
            for (var x = e; x != null; x = x.InnerException)
                parts.Add($"{x.GetType().Name}: {x.Message} (0x{x.HResult:x8})");
            Console.WriteLine($"FAIL {name} [{(cert != null ? Describe(cert) : "no cert")}] {string.Join(" <- ", parts)}");
        }
    }

    static string Describe(X509Certificate2 cert)
    {
        try
        {
            using var key = cert.GetRSAPrivateKey();
            return key switch
            {
                RSACng => "CNG",
                RSACryptoServiceProvider csp => $"CAPI provider={csp.CspKeyContainerInfo.ProviderName}",
                null => "no private key",
                _ => key.GetType().Name,
            };
        }
        catch (Exception e) { return "key? " + e.GetType().Name + ": " + e.Message + $" (0x{e.HResult:x8})"; }
    }
}

static class Native
{
    [System.Runtime.InteropServices.DllImport("advapi32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
    public static extern bool GetUserNameW(System.Text.StringBuilder name, ref uint size);
    [System.Runtime.InteropServices.DllImport("advapi32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode, SetLastError = true)]
    public static extern bool CryptAcquireContextW(out IntPtr prov, string? container, string? provider, uint type, uint flags);
    [System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
    public static extern bool CryptReleaseContext(IntPtr prov, uint flags);
}
