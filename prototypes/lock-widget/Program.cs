// Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
// Read-only Windows widget companion; no alarm, audio, authentication, or lock-screen settings.
// COM registration follows the Microsoft WindowsAppSDK-Samples Widgets sample (MIT).
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text;
using Microsoft.Windows.Widgets.Providers;
using Windows.ApplicationModel;
using Windows.ApplicationModel.AppExtensions;
using WinRT;
using Windows.Storage.Streams;
using Microsoft.Win32.SafeHandles;
using System.Security.Principal;

namespace AlarmWidgetPrototype;

internal static class Program
{
#if ALARM_WIDGET_CANDIDATE
    internal const string PackageName = "Ibrahim.ALARM.Widgets";
    internal const string DefinitionId = "AlarmSmall";
    internal const string WebDefinitionId = "AlarmGlassSmall";
    internal const string ProviderClassId = "A52E88C7-6199-4D4A-8CF9-8B82E657B1F4";
    internal const string IdleExitEvent = @"Local\Ibrahim.ALARM.Widgets.StopIfEmpty";
#else
    internal const string PackageName = "Ibrahim.ALARM.WidgetPrototype";
    internal const string DefinitionId = "AlarmSmallPrototype";
    internal const string WebDefinitionId = "AlarmSmallWebPrototype";
    internal const string ProviderClassId = "F61B9C5D-6D5B-452A-8A3B-941C8098A4D6";
    internal const string IdleExitEvent = @"Local\Ibrahim.ALARM.WidgetPrototype.StopIfEmpty";
#endif
    internal const string WebOrigin = "https://alarm-widget.invalid";
    internal static readonly string DirectoryPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Ibrahim", "ALARM");
    internal static readonly string StateDirectoryPath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".alarm");
    internal static string StatePath = Path.Combine(StateDirectoryPath, "widget-state.json");
    internal static readonly string EvidencePath = Path.Combine(DirectoryPath, "widget-prototype.log");

    [MTAThread]
    public static int Main(string[] args)
    {
        try
        {
            ComWrappersSupport.InitializeComWrappers();
            if (args.Length == 3 && args[0] == "--state-probe")
            {
                StatePath = Path.GetFullPath(args[1]);
                File.WriteAllText(args[2], StateReader.ReadData());
                return 0;
            }
            if (args.Length == 2 && args[0] == "--api-probe") return Probe(args[1]);
            if (args.Length == 2 && args[0] == "--template-probe")
            { File.WriteAllText(args[1], AlarmProvider.Template); return 0; }
            if (args.Length == 3 && args[0] == "--refresh-instance") return RefreshInstance(args[1], args[2]);
            if (args.Length == 0 || args[0] != "-RegisterProcessAsComServer" ||
                args.Skip(1).Any(a => !a.Equals("-Embedding", StringComparison.OrdinalIgnoreCase) &&
                                     !a.Equals("/Embedding", StringComparison.OrdinalIgnoreCase))) return 0;

            using var idleExit = new EventWaitHandle(false, EventResetMode.ManualReset, IdleExitEvent);
            int result = Native.CoRegisterClassObject(new Guid(ProviderClassId),
                new ProviderFactory(), 4, 1, out uint cookie);
            Marshal.ThrowExceptionForHR(result);
            Log("CoRegisterClassObject=S_OK");
            try
            {
                while (true)
                {
                    int signaled = WaitHandle.WaitAny(new WaitHandle[] { AlarmProvider.Empty, idleExit });
                    if (!AlarmProvider.HasWidgets) break;
                    if (signaled == 0) AlarmProvider.Empty.Reset();
                    idleExit.Reset();
                }
            }
            finally { Native.CoRevokeClassObject(cookie); }
            return 0;
        }
        catch (Exception ex) { Log($"FAIL 0x{ex.HResult:X8}: {ex.Message}"); return 1; }
    }

    private static int RefreshInstance(string selectedId, string output)
    {
        var receipt = new List<string> { "UTC=" + DateTime.UtcNow.ToString("O"),
            "Update existing selected Small instance only; no creation, deletion, pin, UI or engine action." };
        try
        {
            receipt.Add("Package=" + Package.Current.Id.FullName);
            var manager = WidgetManager.GetDefault();
            var selected = (manager.GetWidgetInfos() ?? Array.Empty<WidgetInfo>())
                .SingleOrDefault(info => info.WidgetContext.Id == selectedId &&
                    info.WidgetContext.DefinitionId == DefinitionId);
            if (selected == null || selected.WidgetContext.Size.ToString() != "Small")
                throw new InvalidOperationException("The existing selected ALARM Small instance was not found; nothing was updated.");
            receipt.Add("Before=" + JsonSerializer.Serialize(new { selected.WidgetContext.Id,
                selected.WidgetContext.DefinitionId, Size = selected.WidgetContext.Size.ToString(), selected.WidgetContext.IsActive }));
            string data = StateReader.ReadData();
            manager.UpdateWidget(new WidgetUpdateRequestOptions(selectedId) { Template = AlarmProvider.Template, Data = data });
            receipt.Add("UpdateWidget=S_OK id=" + selectedId);
            receipt.Add("StateData=" + data);
            var after = (manager.GetWidgetInfos() ?? Array.Empty<WidgetInfo>()).Single(info => info.WidgetContext.Id == selectedId);
            if (after.WidgetContext.DefinitionId != DefinitionId || after.WidgetContext.Size.ToString() != "Small")
                throw new InvalidOperationException("The selected instance definition/size changed unexpectedly.");
            receipt.Add("After=" + JsonSerializer.Serialize(new { after.WidgetContext.Id,
                after.WidgetContext.DefinitionId, Size = after.WidgetContext.Size.ToString(), after.WidgetContext.IsActive }));
            File.WriteAllLines(output, receipt); return 0;
        }
        catch (Exception ex)
        {
            receipt.Add($"FAIL=0x{ex.HResult:X8}: {ex.Message}");
            File.WriteAllLines(output, receipt); return 1;
        }
    }

    private static int Probe(string output)
    {
        var evidence = new List<string> { "UTC=" + DateTime.UtcNow.ToString("O") };
        try
        {
            evidence.Add("Package=" + Package.Current.Id.FullName);
            evidence.Add("Token=" + Native.TokenDescription());
            evidence.Add("StatePath=" + StatePath);
            evidence.Add("File.Exists(state)=" + File.Exists(StatePath));
            try { evidence.Add("RawEngineState=" + File.ReadAllText(StatePath)); }
            catch (Exception ex) { evidence.Add("ReadEngineState=" + ex.GetType().Name + ": " + ex.Message); }
            var extensions = AppExtensionCatalog.Open("com.microsoft.windows.widgets").FindAllAsync()
                .AsTask().GetAwaiter().GetResult();
            var ours = extensions.Where(x => x.Package.Id.Name == PackageName).ToArray();
            evidence.Add("OwnWidgetExtensions=" + ours.Length);
            foreach (var extension in ours) evidence.Add("Extension=" + extension.Id);
            var manager = WidgetManager.GetDefault();
            var ids = manager.GetWidgetIds() ?? Array.Empty<string>();
            evidence.Add("WidgetManager.GetDefault=S_OK");
            evidence.Add("ExistingUserSelectedInstances=" + ids.Length);
            foreach (var id in ids) evidence.Add("Instance=" + id);
            evidence.Add("StateData=" + StateReader.ReadData());
            var resourceStream = new InMemoryRandomAccessStream();
            using (var resourceWriter = new DataWriter(resourceStream))
            { resourceWriter.WriteBytes(Encoding.UTF8.GetBytes("local-probe")); resourceWriter.StoreAsync().AsTask().GetAwaiter().GetResult(); resourceWriter.DetachStream(); }
            resourceStream.Seek(0);
            var resourceResponse = new WidgetResourceResponse(RandomAccessStreamReference.CreateFromStream(resourceStream), "OK", 200);
            resourceResponse.Headers.Add("Content-Type", "text/plain; charset=utf-8");
            evidence.Add("WidgetResourceResponse(local stream)=S_OK");
            // This activates only the provider COM server, never pins or creates a widget.
            var clsid = new Guid(ProviderClassId);
            var iid = typeof(IWidgetProvider).GUID;
            int hr = Native.CoCreateInstance(ref clsid, IntPtr.Zero, 4, ref iid, out IntPtr instance);
            evidence.Add($"CoCreateInstance(IWidgetProvider)=0x{hr:X8}");
            if (instance != IntPtr.Zero)
            {
                var resourceIid = typeof(IWidgetResourceProvider).GUID;
                int resourceHr = Marshal.QueryInterface(instance, in resourceIid, out IntPtr resource);
                evidence.Add($"QueryInterface(IWidgetResourceProvider)=0x{resourceHr:X8}");
                if (resource != IntPtr.Zero) Marshal.Release(resource);
                var messageIid = typeof(IWidgetProviderMessage).GUID;
                int messageHr = Marshal.QueryInterface(instance, in messageIid, out IntPtr message);
                evidence.Add($"QueryInterface(IWidgetProviderMessage)=0x{messageHr:X8}");
                if (message != IntPtr.Zero) Marshal.Release(message);
            }
            if (instance != IntPtr.Zero) Marshal.Release(instance);
            if (ids.Length == 0 && EventWaitHandle.TryOpenExisting(IdleExitEvent, out var stop))
            { using (stop) stop.Set(); }
            File.WriteAllLines(output, evidence);
            return hr == 0 && ours.Length == 1 ? 0 : 2;
        }
        catch (Exception ex)
        {
            evidence.Add($"FAIL=0x{ex.HResult:X8}: {ex.Message}");
            File.WriteAllLines(output, evidence);
            return 1;
        }
    }

    internal static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(DirectoryPath);
            var info = new FileInfo(EvidencePath);
            if (info.Exists && info.Length > 256 * 1024) return;
            File.AppendAllText(EvidencePath, DateTime.UtcNow.ToString("O") + " " + message + "\n");
        }
        catch { /* Evidence failure must not affect the alarm engine. */ }
    }
}

internal static class StateReader
{
    private static object Unavailable(string detail) => new
    {
        time = "—", status = "ALARM closed", next = detail,
        sound = "Open ALARM to start the engine", color = "warning", phase = "closed"
    };

    internal static string ReadData()
    {
        try
        {
            using var stream = new FileStream(Program.StatePath, FileMode.Open, FileAccess.Read,
                FileShare.ReadWrite | FileShare.Delete);
            if (stream.Length > 64 * 1024) throw new InvalidDataException("State file is too large");
            using var doc = JsonDocument.Parse(stream);
            var root = doc.RootElement;
            int hour = root.GetProperty("hour").GetInt32();
            int minute = root.GetProperty("minute").GetInt32();
            if (hour < 0 || hour > 23 || minute < 0 || minute > 59) throw new InvalidDataException("Invalid time");
            bool enabled = root.GetProperty("enabled").GetBoolean();
            string phase = root.GetProperty("phase").GetString() ?? "";
            if (phase is not ("idle" or "scheduled" or "ringing" or "silent" or "error"))
                throw new InvalidDataException("Unknown phase");
            string updated = root.GetProperty("updatedUtc").GetString() ?? "";
            if (!DateTimeOffset.TryParse(updated, out _)) throw new InvalidDataException("Invalid timestamp");
            int pid = checked((int)root.GetProperty("processId").GetUInt32());
            using var process = Process.GetProcessById(pid);
            if (process.HasExited || process.ProcessName is not ("ALARM_MVP" or "ALARM"))
                return JsonSerializer.Serialize(Unavailable("The ALARM engine is no longer running"));
            string nextUtc = root.GetProperty("nextTransitionUtc").GetString() ?? "";
            string next = "";
            if (nextUtc.Length > 0)
            {
                if (!DateTimeOffset.TryParse(nextUtc, out var deadline)) throw new InvalidDataException("Invalid deadline");
                string label = phase == "ringing" ? "Pause at " : phase == "silent" ? "Resume at " : "Alarm on ";
                next = label + deadline.ToLocalTime().ToString(phase is "ringing" or "silent" ? "HH:mm" : "yyyy-MM-dd 'at' HH:mm");
            }
            string status = phase switch
            {
                "ringing" => "Your alarm is ringing", "silent" => "Ringing paused",
                "error" => "Sound requested: audio error", _ => enabled ? "Alarm enabled" : "Alarm disabled"
            };
            string sound = root.GetProperty("soundLabel").GetString() ?? "";
            if (sound.Length > 80) sound = sound[..80] + "…";
            return JsonSerializer.Serialize(new { time = $"{hour:00}:{minute:00}", status, phase,
                next, sound, color = phase is "ringing" or "error" ? "attention" : enabled ? "accent" : "default" });
        }
        catch (FileNotFoundException ex)
        {
            Program.Log("Missing file/assembly: " + ex.Message);
            return JsonSerializer.Serialize(Unavailable("No engine state available"));
        }
        catch (ArgumentException) { return JsonSerializer.Serialize(Unavailable("The ALARM engine is no longer running")); }
        catch (Exception ex)
        {
            Program.Log("State unavailable: " + ex.GetType().Name);
            return JsonSerializer.Serialize(Unavailable("Engine state unavailable"));
        }
    }
}

[ComVisible(true), ComDefaultInterface(typeof(IWidgetProvider)), Guid(Program.ProviderClassId)]
public sealed class AlarmProvider : IWidgetProvider, IWidgetProviderErrors, IWidgetResourceProvider, IWidgetProviderMessage
{
    internal static readonly ManualResetEvent Empty = new(false);
    private sealed record TrackedWidget(string Definition, bool Active);
    private static readonly ConcurrentDictionary<string, TrackedWidget> Widgets = new();
    internal static bool HasWidgets => !Widgets.IsEmpty;
    private static readonly Lazy<AlarmProvider> Singleton = new(() => new AlarmProvider());
    internal static AlarmProvider Instance => Singleton.Value;
    private FileSystemWatcher? watcher;
    private readonly Timer refresh;
    private readonly Timer debounce;
    private string lastData = "";
    private readonly object updateLock = new();

    // Adaptive Cards lets Windows own layout, accessibility, colors and above-lock activation.
    // No action executes engine changes above lock; opening uses the fixed ALARM protocol.
    private static readonly Lazy<string> VisualTemplate = new(() => TemplateJson
        .Replace("__ALARM_LOGO__", "data:image/png;base64," + Convert.ToBase64String(File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "Assets", "Alarm.png"))))
        .Replace("__ALARM_GLASS__", "data:image/png;base64," + Convert.ToBase64String(File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "Assets", "GlassSurface.png")))));
    internal static string Template => VisualTemplate.Value;
    private const string TemplateJson = """
    {
      "$schema":"http://adaptivecards.io/schemas/adaptive-card.json",
      "type":"AdaptiveCard","version":"1.5",
      "backgroundImage":{"url":"__ALARM_GLASS__","fillMode":"cover","verticalAlignment":"center"},
      "body":[
        {"type":"ColumnSet","spacing":"None","columns":[
          {"type":"Column","width":"stretch","verticalContentAlignment":"Center","items":[
            {"type":"TextBlock","text":"${time}","size":"ExtraLarge","weight":"Bolder","color":"Light","wrap":false,"spacing":"None"}]},
          {"type":"Column","width":"auto","verticalContentAlignment":"Center","spacing":"Small","items":[
            {"type":"Image","url":"__ALARM_LOGO__","width":"28px","height":"28px","altText":"ALARM","spacing":"None"}]}
        ]},
        {"type":"TextBlock","text":"${status}","weight":"Bolder","color":"Light","spacing":"Small","maxLines":1,"wrap":true},
        {"type":"TextBlock","text":"${next}","color":"Light","isSubtle":true,"size":"Small","spacing":"None","maxLines":1,"wrap":true}
      ],
      "selectAction":{"type":"Action.OpenUrl","url":"ibrahim-alarm://open"}
    }
    """;
    private const string WebTemplate = """
    {"$schema":"http://adaptivecards.io/schemas/adaptive-card.json","type":"AdaptiveCard",
     "version":"1.6","body":[],"metadata":{"webUrl":"https://alarm-widget.invalid/index.html"}}
    """;
    private static bool OwnDefinition(string definition) => definition is Program.DefinitionId or Program.WebDefinitionId;

    private AlarmProvider()
    {
        Program.Log("ProviderState=" + StateReader.ReadData());
        Program.Log("ProviderToken=" + Native.TokenDescription());
        debounce = new Timer(_ => UpdateAll(), null, Timeout.Infinite, Timeout.Infinite);
        refresh = new Timer(_ => UpdateAll(), null, Timeout.Infinite, Timeout.Infinite);
        EnsureWatcher();
        try
        {
            foreach (var info in WidgetManager.GetDefault().GetWidgetInfos() ?? Array.Empty<WidgetInfo>())
                if (OwnDefinition(info.WidgetContext.DefinitionId))
                    Widgets.TryAdd(info.WidgetContext.Id, new TrackedWidget(info.WidgetContext.DefinitionId, false));
        }
        catch (Exception ex) { Program.Log($"Restore instances failed=0x{ex.HResult:X8}"); }
    }

    private void EnsureWatcher()
    {
        lock (updateLock)
        {
        if (watcher == null && Directory.Exists(Program.StateDirectoryPath))
        {
            watcher = new FileSystemWatcher(Program.StateDirectoryPath, "widget-state.json")
            { NotifyFilter = NotifyFilters.FileName | NotifyFilters.LastWrite | NotifyFilters.Size };
            watcher.Changed += (_, _) => debounce.Change(250, Timeout.Infinite);
            watcher.Created += (_, _) => debounce.Change(250, Timeout.Infinite);
            watcher.Renamed += (_, _) => debounce.Change(250, Timeout.Infinite);
            watcher.Deleted += (_, _) => debounce.Change(250, Timeout.Infinite);
        }
        }
    }

    public void CreateWidget(WidgetContext context)
    {
        if (!OwnDefinition(context.DefinitionId)) return;
        string id = context.Id;
        Empty.Reset(); Widgets[id] = new TrackedWidget(context.DefinitionId, true);
        UpdateObservation();
        Program.Log("CreateWidget=" + id); UpdateOne(id);
    }
    public void DeleteWidget(string widgetId, string customState)
    {
        Widgets.TryRemove(widgetId, out _); Program.Log("DeleteWidget=" + widgetId);
        UpdateObservation();
        if (Widgets.IsEmpty) Empty.Set();
    }
    public void Activate(WidgetContext context)
    {
        if (!OwnDefinition(context.DefinitionId)) return;
        string id = context.Id;
        Empty.Reset(); Widgets[id] = new TrackedWidget(context.DefinitionId, true); Program.Log("Activate=" + id); UpdateOne(id);
        UpdateObservation();
    }
    public void Deactivate(string widgetId)
    {
        if (Widgets.TryGetValue(widgetId, out var widget))
        {
            Widgets[widgetId] = widget with { Active = false };
            if (widget.Definition == Program.WebDefinitionId) SendVisibility(widgetId, false);
        }
        UpdateObservation();
    }
    public void OnWidgetContextChanged(WidgetContextChangedArgs args) => UpdateOne(args.WidgetContext.Id);
    public void OnActionInvoked(WidgetActionInvokedArgs args) => UpdateOne(args.WidgetContext.Id);
    public void OnErrorInfoReported(WidgetErrorInfoReportedArgs args)
    {
        string error = args.ErrorJson;
        if (error.Length > 1024) error = error[..1024];
        Program.Log("HostError=" + error.Replace("\r", " ").Replace("\n", " "));
    }
    public void OnMessageReceived(WidgetMessageReceivedArgs args)
    {
        // The local web page can request presentation data, never operate the engine.
        if (args.WidgetContext.DefinitionId == Program.WebDefinitionId && args.Message == "ready")
        {
            string id = args.WidgetContext.Id;
            if (Widgets.TryGetValue(id, out var widget)) SendVisibility(id, widget.Active);
            WidgetManager.GetDefault().SendMessageToContent(id, StateReader.ReadData());
        }
    }

    public async void OnResourceRequested(WidgetResourceRequestedArgs args)
    {
        var deferral = args.GetDeferral();
        string requestedUri = args.Request.Uri;
        try
        {
            byte[] payload = Array.Empty<byte>();
            string contentType = "text/plain; charset=utf-8";
            int status = 404;
            if (Uri.TryCreate(requestedUri, UriKind.Absolute, out var uri) &&
                uri.Scheme == "https" && uri.Host == "alarm-widget.invalid" && uri.IsDefaultPort &&
                uri.UserInfo.Length == 0 && uri.Query.Length == 0)
            {
                string? file = uri.AbsolutePath switch
                {
                    "/index.html" => "index.html", "/widget.css" => "widget.css", "/widget.js" => "widget.js", "/alarm.png" => "alarm.png", _ => null
                };
                if (uri.AbsolutePath == "/state.json")
                { payload = Encoding.UTF8.GetBytes(StateReader.ReadData()); contentType = "application/json; charset=utf-8"; status = 200; }
                else if (file != null)
                {
                    payload = File.ReadAllBytes(Path.Combine(AppContext.BaseDirectory, "Web", file));
                    contentType = file.EndsWith(".png") ? "image/png" : file.EndsWith(".html") ? "text/html; charset=utf-8" :
                                  file.EndsWith(".css") ? "text/css; charset=utf-8" : "text/javascript; charset=utf-8";
                    status = 200;
                }
            }
            var stream = new InMemoryRandomAccessStream();
            using (var writer = new DataWriter(stream))
            { writer.WriteBytes(payload); await writer.StoreAsync(); writer.DetachStream(); }
            stream.Seek(0);
            var response = new WidgetResourceResponse(RandomAccessStreamReference.CreateFromStream(stream),
                status == 200 ? "OK" : "Not Found", status);
            response.Headers.Add("Content-Type", contentType);
            response.Headers.Add("Cache-Control", "no-store");
            response.Headers.Add("X-Content-Type-Options", "nosniff");
            response.Headers.Add("Content-Security-Policy", WebPolicy);
            args.Response = response;
            Program.Log("Local resource response=" + status + " path=" + new Uri(requestedUri).AbsolutePath);
        }
        catch (Exception ex)
        {
            Program.Log($"Resource failed=0x{ex.HResult:X8}");
            // Still provide a local response so the widget host does not fetch the URI.
            var empty = new InMemoryRandomAccessStream();
            args.Response = new WidgetResourceResponse(RandomAccessStreamReference.CreateFromStream(empty), "Unavailable", 503);
            args.Response.Headers.Add("Content-Security-Policy", WebPolicy);
            args.Response.Headers.Add("Content-Type", "text/plain; charset=utf-8");
            args.Response.Headers.Add("Cache-Control", "no-store");
        }
        finally { deferral.Complete(); }
    }
    private const string WebPolicy = "default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; " +
        "img-src 'self' data:; font-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; " +
        "base-uri 'none'; form-action 'none'";
    private void UpdateObservation()
    {
        lock (updateLock)
        {
            bool active = Widgets.Values.Any(w => w.Active);
            if (active) EnsureWatcher();
            refresh.Change(active ? 30000 : Timeout.Infinite, active ? 30000 : Timeout.Infinite);
            if (watcher != null) watcher.EnableRaisingEvents = active;
            if (!active) debounce.Change(Timeout.Infinite, Timeout.Infinite);
        }
    }
    private void SendVisibility(string id, bool active)
    {
        try { WidgetManager.GetDefault().SendMessageToContent(id, active ? "{\"hostActive\":true}" : "{\"hostActive\":false}"); }
        catch (Exception ex) { Program.Log($"Visibility message failed=0x{ex.HResult:X8}"); }
    }

    private void UpdateOne(string id)
    {
        lock (updateLock)
        {
            try
            {
                if (!Widgets.TryGetValue(id, out var widget)) return;
                string data = StateReader.ReadData();
                var request = new WidgetUpdateRequestOptions(id)
                { Template = widget.Definition == Program.WebDefinitionId ? WebTemplate : Template, Data = data };
                WidgetManager.GetDefault().UpdateWidget(request);
                if (widget.Definition == Program.WebDefinitionId)
                { SendVisibility(id, widget.Active); WidgetManager.GetDefault().SendMessageToContent(id, data); }
                Program.Log("UpdateWidget=S_OK id=" + id);
            }
            catch (Exception ex) { Program.Log($"UpdateWidget failed=0x{ex.HResult:X8} id={id}"); }
        }
    }
    private void UpdateAll()
    {
        lock (updateLock)
        {
            if (!Widgets.Values.Any(w => w.Active)) return;
            EnsureWatcher();
            if (watcher != null) watcher.EnableRaisingEvents = true;
            string data = StateReader.ReadData();
            if (data == lastData) return;
            bool complete = true;
            foreach (var widget in Widgets.ToArray())
            {
                if (!widget.Value.Active) continue;
                try
                {
                    WidgetManager.GetDefault().UpdateWidget(new WidgetUpdateRequestOptions(widget.Key)
                    { Template = widget.Value.Definition == Program.WebDefinitionId ? WebTemplate : Template, Data = data });
                    if (widget.Value.Definition == Program.WebDefinitionId)
                        WidgetManager.GetDefault().SendMessageToContent(widget.Key, data);
                    Program.Log("UpdateWidget=S_OK id=" + widget.Key);
                }
                catch (Exception ex) { complete = false; Program.Log($"UpdateWidget failed=0x{ex.HResult:X8} id={widget.Key}"); }
            }
            // A transient host failure must be retried on the next active refresh.
            if (complete) lastData = data;
        }
    }
}

[ComImport, InterfaceType(ComInterfaceType.InterfaceIsIUnknown), Guid("00000001-0000-0000-C000-000000000046")]
internal interface IClassFactory
{
    [PreserveSig] int CreateInstance(IntPtr outer, ref Guid iid, out IntPtr instance);
    [PreserveSig] int LockServer([MarshalAs(UnmanagedType.Bool)] bool locked);
}

internal sealed class ProviderFactory : IClassFactory
{
    public int CreateInstance(IntPtr outer, ref Guid iid, out IntPtr instance)
    {
        instance = IntPtr.Zero;
        if (outer != IntPtr.Zero) return unchecked((int)0x80040110);
        IntPtr unknown = IntPtr.Zero;
        try
        {
            unknown = MarshalInspectable<IWidgetProvider>.FromManaged(AlarmProvider.Instance);
            return Marshal.QueryInterface(unknown, in iid, out instance);
        }
        catch (Exception ex) { Program.Log($"Factory failed=0x{ex.HResult:X8}"); return ex.HResult; }
        finally { if (unknown != IntPtr.Zero) Marshal.Release(unknown); }
    }
    public int LockServer(bool locked) => 0;
}

internal static class Native
{
    internal static string TokenDescription()
    {
        using var identity = WindowsIdentity.GetCurrent();
        if (!GetTokenInformation(identity.AccessToken, 29, out int container, sizeof(int), out _))
            return "TokenIsAppContainer query failed=" + Marshal.GetLastWin32Error();
        GetTokenInformationBuffer(identity.AccessToken, 25, IntPtr.Zero, 0, out int size);
        IntPtr buffer = Marshal.AllocHGlobal(size);
        try
        {
            if (!GetTokenInformationBuffer(identity.AccessToken, 25, buffer, size, out _))
                return "TokenIsAppContainer=" + container + " Integrity query failed=" + Marshal.GetLastWin32Error();
            IntPtr sid = Marshal.ReadIntPtr(buffer);
            byte count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            uint level = unchecked((uint)Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1))));
            return "TokenIsAppContainer=" + container + "; IntegrityRID=" + level;
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformation(SafeAccessTokenHandle token, int informationClass,
        out int information, int length, out int returnLength);
    [DllImport("advapi32.dll", EntryPoint="GetTokenInformation", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetTokenInformationBuffer(SafeAccessTokenHandle token, int informationClass,
        IntPtr information, int length, out int returnLength);
    [DllImport("advapi32.dll")] private static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")] private static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    [DllImport("ole32.dll")]
    internal static extern int CoRegisterClassObject([MarshalAs(UnmanagedType.LPStruct)] Guid clsid,
        [MarshalAs(UnmanagedType.IUnknown)] object factory, uint context, uint flags, out uint cookie);
    [DllImport("ole32.dll")] internal static extern int CoRevokeClassObject(uint cookie);
    [DllImport("ole32.dll")]
    internal static extern int CoCreateInstance(ref Guid clsid, IntPtr outer, uint context, ref Guid iid, out IntPtr instance);
}
