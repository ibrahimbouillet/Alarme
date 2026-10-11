// Copyright (c) 2026 Ibrahim Bouillet. SPDX-License-Identifier: MIT
// Inspect existing widgets without changing their state or selection.
using Microsoft.Windows.Widgets.Providers;
using Windows.ApplicationModel;
using WinRT;
using System.Text.Json;

internal static class Program
{
    [MTAThread]
    public static int Main(string[] args)
    {
        if (args.Length != 1) return 2;
        var receipt = new List<string> { "UTC=" + DateTime.UtcNow.ToString("O"),
            "Read-only GetWidgetInfos; no creation, update, deletion, pin, UI or engine action." };
        try
        {
            ComWrappersSupport.InitializeComWrappers();
            receipt.Add("Package=" + Package.Current.Id.FullName);
            var infos = WidgetManager.GetDefault().GetWidgetInfos() ?? Array.Empty<WidgetInfo>();
            receipt.Add("GetWidgetInfos=S_OK; Count=" + infos.Length);
            foreach (var info in infos)
            {
                var context = info.WidgetContext;
                receipt.Add(JsonSerializer.Serialize(new { context.Id, context.DefinitionId,
                    context.IsActive, Size = context.Size.ToString() }));
            }
            File.WriteAllLines(args[0], receipt);
            return 0;
        }
        catch (Exception ex)
        {
            receipt.Add($"FAIL=0x{ex.HResult:X8}: {ex.Message}");
            File.WriteAllLines(args[0], receipt); return 1;
        }
    }
}
