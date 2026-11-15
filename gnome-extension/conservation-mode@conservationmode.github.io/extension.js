// Quick Settings toggle for Lenovo IdeaPad battery Conservation Mode.
//
// Reads /sys/bus/platform/drivers/ideapad_acpi/*/conservation_mode directly
// (world-readable). Changes go through a root-owned helper started with
// pkexec; the polkit policy installed with it lets the active local user run
// it without a password.

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import {QuickToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const DRIVER_DIR = '/sys/bus/platform/drivers/ideapad_acpi';
// .deb package installs to /usr/libexec; install-helper.sh to /usr/local/libexec.
const HELPERS = [
    '/usr/libexec/conservation-mode-helper',
    '/usr/local/libexec/conservation-mode-helper',
];
const ICON = 'battery-good-charging-symbolic';

function findSysfsFile() {
    try {
        const dir = Gio.File.new_for_path(DRIVER_DIR);
        const children = dir.enumerate_children('standard::name', Gio.FileQueryInfoFlags.NONE, null);
        let info;
        while ((info = children.next_file(null)) !== null) {
            const f = dir.get_child(info.get_name()).get_child('conservation_mode');
            if (f.query_exists(null))
                return f.get_path();
        }
    } catch (e) {
        // Driver not loaded.
    }
    return null;
}

function readState(path) {
    try {
        const [ok, bytes] = GLib.file_get_contents(path);
        if (ok)
            return new TextDecoder().decode(bytes).trim() === '1';
    } catch (e) {
        logError(e, 'Conservation Mode: read failed');
    }
    return null;
}

const ConservationToggle = GObject.registerClass(
class ConservationToggle extends QuickToggle {
    _init() {
        super._init({
            title: 'Conservation',
            subtitle: 'Battery charge limit',
            iconName: ICON,
            toggleMode: false, // checked state only follows the real sysfs value
        });
        this._path = findSysfsFile();
        this._busy = false;
        this.visible = this._path !== null;
        this.connect('clicked', () => this._onClicked());
        this.sync();
    }

    sync() {
        if (!this._path)
            return;
        const state = readState(this._path);
        if (state !== null)
            this.checked = state;
    }

    _onClicked() {
        if (this._busy || !this._path)
            return;
        const want = !this.checked;
        this._busy = true;
        this.reactive = false;

        const helper = HELPERS.find(p => GLib.file_test(p, GLib.FileTest.IS_EXECUTABLE));
        if (!helper) {
            this._finish('Helper not found. Install the conservation-mode .deb, or run: sudo gnome-extension/install-helper.sh');
            return;
        }

        let proc;
        try {
            proc = Gio.Subprocess.new(['pkexec', helper, want ? 'on' : 'off'],
                Gio.SubprocessFlags.STDERR_PIPE);
        } catch (e) {
            this._finish(`Could not start pkexec: ${e.message}`);
            return;
        }
        proc.communicate_utf8_async(null, null, (p, res) => {
            let err = null;
            try {
                const [, , stderr] = p.communicate_utf8_finish(res);
                const code = p.get_exit_status();
                if (code === 127 || code === 126)
                    err = 'Authorization failed or was cancelled.';
                else if (code !== 0)
                    err = (stderr || '').trim() || `Helper exited with status ${code}`;
            } catch (e) {
                err = e.message;
            }
            this._finish(err);
        });
    }

    _finish(err) {
        this._busy = false;
        this.reactive = true;
        this.sync();
        if (err)
            Main.notify('Conservation Mode', err);
    }
});

const ConservationIndicator = GObject.registerClass(
class ConservationIndicator extends SystemIndicator {
    _init() {
        super._init();
        this._indicator = this._addIndicator();
        this._indicator.iconName = ICON;

        this.toggle = new ConservationToggle();
        this.toggle.bind_property('checked', this._indicator, 'visible',
            GObject.BindingFlags.SYNC_CREATE);
        this.quickSettingsItems.push(this.toggle);
    }

    destroy() {
        this.quickSettingsItems.forEach(item => item.destroy());
        super.destroy();
    }
});

export default class ConservationModeExtension extends Extension {
    enable() {
        this._indicator = new ConservationIndicator();
        const qs = Main.panel.statusArea.quickSettings;
        qs.addExternalIndicator(this._indicator);
        // Show the current state every time the menu opens (it can change
        // from the GTK app, the command line, or firmware).
        this._menuId = qs.menu.connect('open-state-changed', (_m, open) => {
            if (open)
                this._indicator.toggle.sync();
        });
    }

    disable() {
        Main.panel.statusArea.quickSettings.menu.disconnect(this._menuId);
        this._menuId = 0;
        this._indicator.destroy();
        this._indicator = null;
    }
}
