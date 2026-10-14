// NFSU2 Mod Settings - edits the VR mod (NFSU2HeadTrack.ini) and the telemetry / force feedback /
// wheel input mod (NFSU2Telemetry.ini) without touching the ini files by hand.
//
// The ini files stay the source of truth: every key found in them is shown (so new settings
// appear automatically), comments next to a key become its description, and saving only
// replaces the values, keeping comments and layout. Lives in the game folder and reads .\scripts.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;

static class Program
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new SettingsForm());
    }
}

// ---------------------------------------------------------------- ini model
public class IniEntry
{
    public string Section, Key, Value, Comment;
    public int Line;
}

public class IniFile
{
    public string Path;
    public List<string> Lines = new List<string>();
    public List<IniEntry> Entries = new List<IniEntry>();
    public Dictionary<string, string> SectionNotes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
    public string HeaderNote = "";

    static readonly Regex KeyLine = new Regex(@"^(\s*)([^;=\[\s][^=]*?)(\s*=\s*)([^;]*?)(\s*)(;.*)?$");

    public static IniFile Load(string path)
    {
        var f = new IniFile { Path = path };
        f.Lines = File.ReadAllLines(path, Encoding.Default).ToList();
        // Comment lines are attached to what follows them: before a section header or right
        // after it they describe the section, otherwise they describe the next key.
        string section = "";
        bool sectionHasKeys = false;
        var pending = new List<string>();
        for (int i = 0; i < f.Lines.Count; i++)
        {
            string l = f.Lines[i].Trim();
            if (l.StartsWith("[") && l.Contains("]"))
            {
                section = l.Substring(1, l.IndexOf(']') - 1);
                sectionHasKeys = false;
                if (pending.Count > 0) { f.SectionNotes[section] = string.Join(" ", pending); pending.Clear(); }
                continue;
            }
            if (l.StartsWith(";")) { pending.Add(l.TrimStart(';').Trim()); continue; }
            var m = KeyLine.Match(f.Lines[i]);
            if (!m.Success || section == "") continue;
            string inline = m.Groups[6].Success ? m.Groups[6].Value.TrimStart(';').Trim() : "";
            string above = "";
            if (pending.Count > 0)
            {
                if (!sectionHasKeys)
                    f.SectionNotes[section] = (f.SectionNotes.ContainsKey(section) ? f.SectionNotes[section] + " " : "") + string.Join(" ", pending);
                else
                    above = string.Join(" ", pending);
                pending.Clear();
            }
            sectionHasKeys = true;
            f.Entries.Add(new IniEntry
            {
                Section = section,
                Key = m.Groups[2].Value.Trim(),
                Value = m.Groups[4].Value.Trim(),
                Comment = (above + " " + inline).Trim(),
                Line = i
            });
        }
        return f;
    }

    // Replaces only the value, keeping the column where an inline comment starts.
    public void Save(Dictionary<IniEntry, string> values)
    {
        foreach (var kv in values)
        {
            var e = kv.Key;
            var m = KeyLine.Match(Lines[e.Line]);
            if (!m.Success) continue;
            string oldValue = m.Groups[4].Value + m.Groups[5].Value;
            string newValue = kv.Value;
            if (m.Groups[6].Success)
                newValue = newValue.Length < oldValue.Length ? newValue.PadRight(oldValue.Length) : newValue + " ";
            Lines[e.Line] = m.Groups[1].Value + m.Groups[2].Value + m.Groups[3].Value + newValue + (m.Groups[6].Success ? m.Groups[6].Value : "");
            e.Value = kv.Value;
        }
        File.Copy(Path, Path + ".bak", true);
        File.WriteAllLines(Path, Lines, Encoding.Default);
    }
}

// ---------------------------------------------------------------- what each key looks like
static class Meta
{
    // 0/1 switches
    static readonly HashSet<string> Bools = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
    {
        "MAIN.Enabled", "MAIN.OnlyInRace", "MAIN.HideCursor", "SEAT.Cockpit",
        "TRACKING.UsePosition", "TRACKING.UseRoll", "TRACKING.TestMotion",
        "STEREO.AlternateEyes", "OVERLAY.Enabled", "OVERLAY.WorldLock", "OVERLAY.VRFrames",
        "OVERLAY.AutoWidth", "OVERLAY.SideBySide3D", "OVERLAY.BlackBackground",
        "HUD.Enabled", "GAUGES.Enabled", "GAUGES.Map",
        "SIMHUB.Enabled", "FFB.Enabled", "FFB.Invert",
        "INPUT.Enabled", "INPUT.Steering", "INPUT.SteerInvert",
    };

    // fixed choices: value -> label
    public static readonly Dictionary<string, string[][]> Choices = new Dictionary<string, string[][]>(StringComparer.OrdinalIgnoreCase)
    {
        { "STEREO.Mode", new[] { new[] { "1", "Both eyes every frame (depth based)" }, new[] { "0", "Eyes take turns (half frame rate per eye)" } } },
        { "OVERLAY.Transport", new[] { new[] { "0", "GPU hand-off (with DXVK)" }, new[] { "1", "Desktop capture" } } },
        { "INPUT.ThrottleAxis", Axes() }, { "INPUT.BrakeAxis", Axes() }, { "INPUT.HandbrakeAxis", Axes() },
    };

    static string[][] Axes()
    {
        return new[] { "", "X", "Y", "Z", "RX", "RY", "RZ", "S0", "S1" }.Select(a => new[] { a, a == "" ? "(not used)" : a }).ToArray();
    }

    public static bool IsBool(IniEntry e) { return Bools.Contains(e.Section + "." + e.Key); }

    public static string KeyName(string code)
    {
        int v;
        if (!int.TryParse(code, out v)) return "";
        try { return ((Keys)v).ToString(); } catch { return ""; }
    }
}

// ---------------------------------------------------------------- window
public class SettingsForm : Form
{
    readonly string gameDir;
    public static string ForcedGameDir;
    IniFile vr, tele;
    readonly Dictionary<IniEntry, Func<string>> readers = new Dictionary<IniEntry, Func<string>>();
    readonly Dictionary<IniEntry, IniFile> owner = new Dictionary<IniEntry, IniFile>();
    readonly TabControl tabs = new TabControl { Dock = DockStyle.Fill };
    readonly Label status = new Label { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, AutoEllipsis = true };
    readonly ToolTip tip = new ToolTip { AutoPopDelay = 30000 };

    // which sections go on which tab
    static readonly string[][] TabLayout =
    {
        new[] { "VR", "vr:MAIN", "vr:SEAT", "vr:TRACKING", "vr:STEREO", "vr:KEYS" },
        new[] { "VR screen", "vr:OVERLAY" },
        new[] { "VR HUD & gauges", "vr:HUD", "vr:GAUGES" },
        new[] { "Force feedback", "tele:FFB" },
        new[] { "Wheel & pedals", "tele:INPUT" },
        new[] { "SimHub", "tele:SIMHUB" },
    };

    public SettingsForm()
    {
        Text = "NFSU2 Mod Settings";
        Font = new Font("Segoe UI", 9f);
        Size = new Size(820, 700);
        MinimumSize = new Size(640, 480);
        StartPosition = FormStartPosition.CenterScreen;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        gameDir = ForcedGameDir ?? FindGameDir();

        var bottom = new TableLayoutPanel { Dock = DockStyle.Bottom, Height = 44, ColumnCount = 5, Padding = new Padding(8, 6, 8, 6) };
        bottom.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < 4; i++) bottom.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        var save = new Button { Text = "Save", AutoSize = true, Width = 90 };
        var reload = new Button { Text = "Reload", AutoSize = true, Width = 90 };
        var folder = new Button { Text = "Open scripts folder", AutoSize = true };
        var close = new Button { Text = "Close", AutoSize = true, Width = 90 };
        save.Click += (s, e) => SaveAll();
        reload.Click += (s, e) => LoadAll();
        folder.Click += (s, e) => Process.Start("explorer.exe", "\"" + Path.Combine(gameDir, "scripts") + "\"");
        close.Click += (s, e) => Close();
        bottom.Controls.Add(status, 0, 0);
        bottom.Controls.Add(save, 1, 0);
        bottom.Controls.Add(reload, 2, 0);
        bottom.Controls.Add(folder, 3, 0);
        bottom.Controls.Add(close, 4, 0);
        AcceptButton = save;

        Controls.Add(tabs);
        Controls.Add(bottom);
        LoadAll();
    }

    static string FindGameDir()
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        for (int i = 0; i < 4 && dir != null; i++)
        {
            if (File.Exists(Path.Combine(dir, "scripts", "NFSU2HeadTrack.ini")) || File.Exists(Path.Combine(dir, "scripts", "NFSU2Telemetry.ini"))) return dir;
            dir = Path.GetDirectoryName(dir.TrimEnd('\\'));
        }
        using (var d = new FolderBrowserDialog { Description = "Select the Need for Speed Underground 2 folder" })
            if (d.ShowDialog() == DialogResult.OK) return d.SelectedPath;
        Environment.Exit(0);
        return null;
    }

    void LoadAll()
    {
        string vrPath = Path.Combine(gameDir, "scripts", "NFSU2HeadTrack.ini");
        string telePath = Path.Combine(gameDir, "scripts", "NFSU2Telemetry.ini");
        vr = File.Exists(vrPath) ? IniFile.Load(vrPath) : null;
        tele = File.Exists(telePath) ? IniFile.Load(telePath) : null;
        readers.Clear();
        owner.Clear();
        tabs.SuspendLayout();
        int sel = tabs.SelectedIndex;
        tabs.TabPages.Clear();
        var used = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var tab in TabLayout)
        {
            var sections = new List<KeyValuePair<IniFile, string>>();
            foreach (var spec in tab.Skip(1))
            {
                var file = spec.StartsWith("vr:") ? vr : tele;
                string sec = spec.Substring(spec.IndexOf(':') + 1);
                used.Add(spec);
                if (file != null && file.Entries.Any(e => e.Section.Equals(sec, StringComparison.OrdinalIgnoreCase)))
                    sections.Add(new KeyValuePair<IniFile, string>(file, sec));
            }
            if (sections.Count > 0) tabs.TabPages.Add(BuildTab(tab[0], sections));
        }
        // sections a future mod version adds show up on an extra tab
        var extra = new List<KeyValuePair<IniFile, string>>();
        foreach (var f in new[] { vr, tele })
        {
            if (f == null) continue;
            string prefix = f == vr ? "vr:" : "tele:";
            foreach (var sec in f.Entries.Select(e => e.Section).Distinct(StringComparer.OrdinalIgnoreCase))
                if (!used.Contains(prefix + sec)) extra.Add(new KeyValuePair<IniFile, string>(f, sec));
        }
        if (extra.Count > 0) tabs.TabPages.Add(BuildTab("Other", extra));
        if (sel >= 0 && sel < tabs.TabCount) tabs.SelectedIndex = sel;
        tabs.ResumeLayout();

        var missing = new List<string>();
        if (vr == null) missing.Add("NFSU2HeadTrack.ini");
        if (tele == null) missing.Add("NFSU2Telemetry.ini");
        SetStatus(missing.Count > 0 ? "Not found in scripts: " + string.Join(", ", missing) : "Loaded from " + Path.Combine(gameDir, "scripts"));
    }

    TabPage BuildTab(string title, List<KeyValuePair<IniFile, string>> sections)
    {
        var page = new TabPage(title) { Padding = new Padding(4) };
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        var stack = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 1, Padding = new Padding(4) };
        stack.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        foreach (var s in sections) stack.Controls.Add(BuildSection(s.Key, s.Value));
        scroll.Controls.Add(stack);
        scroll.Resize += (o, e) => stack.Width = scroll.ClientSize.Width - 4;
        page.Controls.Add(scroll);
        return page;
    }

    Control BuildSection(IniFile file, string section)
    {
        var box = new GroupBox { Text = section, Dock = DockStyle.Top, AutoSize = true, Padding = new Padding(8, 6, 8, 8), Margin = new Padding(0, 0, 0, 10) };
        var grid = new TableLayoutPanel { Dock = DockStyle.Top, AutoSize = true, ColumnCount = 3, Padding = new Padding(0, 0, 0, 12) };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 160));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 230));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        int row = 0;
        string note;
        if (file.SectionNotes.TryGetValue(section, out note))
        {
            var n = new Label { Text = note, AutoSize = true, MaximumSize = new Size(720, 0), ForeColor = SystemColors.GrayText, Margin = new Padding(3, 0, 3, 8) };
            grid.Controls.Add(n, 0, row);
            grid.SetColumnSpan(n, 3);
            row++;
        }
        foreach (var e in file.Entries.Where(x => x.Section.Equals(section, StringComparison.OrdinalIgnoreCase)))
        {
            owner[e] = file;
            var label = new Label { Text = e.Key, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 6, 3, 3) };
            Control editor = BuildEditor(e);
            var desc = new Label { Text = Describe(e), AutoSize = true, MaximumSize = new Size(380, 0), ForeColor = SystemColors.GrayText, Anchor = AnchorStyles.Left, Margin = new Padding(6, 6, 3, 3) };
            if (desc.Text.Length > 0) { tip.SetToolTip(label, desc.Text); tip.SetToolTip(editor, desc.Text); }
            grid.Controls.Add(label, 0, row);
            grid.Controls.Add(editor, 1, row);
            grid.Controls.Add(desc, 2, row);
            row++;
        }
        box.Controls.Add(grid);
        return box;
    }

    static string Describe(IniEntry e)
    {
        string d = e.Comment;
        if (e.Section.Equals("KEYS", StringComparison.OrdinalIgnoreCase))
        {
            string k = Meta.KeyName(e.Value);
            if (k.Length > 0) d = (d.Length > 0 ? d + "  " : "") + "(currently " + k + ")";
        }
        return d;
    }

    Control BuildEditor(IniEntry e)
    {
        string id = e.Section + "." + e.Key;
        if (Meta.IsBool(e) && (e.Value == "0" || e.Value == "1"))
        {
            var c = new CheckBox { Checked = e.Value == "1", Text = e.Value == "1" ? "On" : "Off", AutoSize = true, Anchor = AnchorStyles.Left };
            c.CheckedChanged += (s, a) => c.Text = c.Checked ? "On" : "Off";
            readers[e] = () => c.Checked ? "1" : "0";
            return c;
        }
        string[][] choices;
        if (Meta.Choices.TryGetValue(id, out choices))
        {
            var cb = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220, Anchor = AnchorStyles.Left };
            var list = choices.ToList();
            if (!list.Any(c => c[0].Equals(e.Value, StringComparison.OrdinalIgnoreCase))) list.Add(new[] { e.Value, e.Value });
            foreach (var c in list) cb.Items.Add(c[1]);
            cb.SelectedIndex = list.FindIndex(c => c[0].Equals(e.Value, StringComparison.OrdinalIgnoreCase));
            readers[e] = () => list[Math.Max(0, cb.SelectedIndex)][0];
            return cb;
        }
        var t = new TextBox { Text = e.Value, Width = 220, Anchor = AnchorStyles.Left };
        bool numeric = IsNumber(e.Value);
        if (numeric)
            t.TextChanged += (s, a) => t.BackColor = IsNumber(t.Text) ? SystemColors.Window : Color.MistyRose;
        if (e.Section.Equals("KEYS", StringComparison.OrdinalIgnoreCase))
        {
            // press a key in the box to set it
            t.ReadOnly = true;
            t.BackColor = SystemColors.Window;
            t.Text = e.Value + "  " + Meta.KeyName(e.Value);
            string code = e.Value;
            t.KeyDown += (s, a) => { code = ((int)a.KeyCode).ToString(); t.Text = code + "  " + a.KeyCode; a.SuppressKeyPress = true; a.Handled = true; };
            tip.SetToolTip(t, "Click here and press the key you want");
            readers[e] = () => code;
            return t;
        }
        readers[e] = () => t.Text.Trim();
        return t;
    }

    static bool IsNumber(string s)
    {
        double d;
        return double.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out d);
    }

    void SaveAll()
    {
        // numbers must stay numbers (and use a dot, not a comma)
        foreach (var kv in readers)
        {
            string v = kv.Value();
            if (IsNumber(kv.Key.Value) && !IsNumber(v))
            {
                MessageBox.Show(this, kv.Key.Section + " / " + kv.Key.Key + ": \"" + v + "\" is not a number (use a dot for decimals).", Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }
        }
        int changed = 0;
        foreach (var f in new[] { vr, tele })
        {
            if (f == null) continue;
            var values = new Dictionary<IniEntry, string>();
            foreach (var kv in readers)
                if (owner[kv.Key] == f && kv.Value() != kv.Key.Value) values[kv.Key] = kv.Value();
            if (values.Count == 0) continue;
            try { f.Save(values); changed += values.Count; }
            catch (Exception ex) { MessageBox.Show(this, "Could not save " + f.Path + ":\n" + ex.Message, Text, MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
        }
        bool running = Process.GetProcessesByName("speed2").Length > 0;
        string msg = changed == 0 ? "Nothing changed." :
            "Saved " + changed + " setting" + (changed == 1 ? "" : "s") + (running ? ". The game is running: restart it to apply." : ". Applies on the next game start.");
        if (changed > 0) LoadAll();
        SetStatus(msg);
    }

    void SetStatus(string s) { status.Text = s; }
}
