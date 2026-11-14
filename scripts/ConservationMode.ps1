# Lenovo Conservation Mode (PowerShell, no build needed)
#
# Command-line alternative to ConservationMode.exe. Talks to the Lenovo
# "ACPI-Compliant Virtual Power Controller" driver (\\.\EnergyDrv),
# the same way Lenovo Vantage does. No admin rights needed.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File ConservationMode.ps1 -Status
#   powershell -ExecutionPolicy Bypass -File ConservationMode.ps1 -On
#   powershell -ExecutionPolicy Bypass -File ConservationMode.ps1 -Off

param([switch]$Status, [switch]$On, [switch]$Off)

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class LenovoEnergy {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFile(string name, uint access, uint share, IntPtr sec, uint disp, uint flags, IntPtr tmpl);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool DeviceIoControl(IntPtr h, uint code, ref uint inBuf, int inSize, out uint outBuf, int outSize, out int returned, IntPtr ov);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr h);

    const uint IOCTL_BATTERY_CHARGE_MODE = 0x831020F8;
    const uint CMD_READ = 0xFF, CMD_CONSERVATION_ON = 0x03, CMD_CONSERVATION_OFF = 0x05;

    static uint Call(uint input) {
        IntPtr h = CreateFile("\\\\.\\EnergyDrv", 0xC0000000, 3, IntPtr.Zero, 3, 0x80, IntPtr.Zero);
        if (h == new IntPtr(-1))
            throw new Exception("Lenovo driver (EnergyDrv) not found. Error " + Marshal.GetLastWin32Error());
        try {
            uint output; int ret;
            if (!DeviceIoControl(h, IOCTL_BATTERY_CHARGE_MODE, ref input, 4, out output, 4, out ret, IntPtr.Zero))
                throw new Exception("The Lenovo driver rejected the command. Error " + Marshal.GetLastWin32Error());
            return output;
        } finally { CloseHandle(h); }
    }

    // Bit 5 of the status value = conservation mode is on
    public static bool IsConservationOn() { return (Call(CMD_READ) & 0x20) != 0; }
    public static void SetConservation(bool on) { Call(on ? CMD_CONSERVATION_ON : CMD_CONSERVATION_OFF); }
}
"@

if (-not ($Status -or $On -or $Off)) { $Status = $true }
if ($On)  { [LenovoEnergy]::SetConservation($true);  Start-Sleep -Milliseconds 300 }
if ($Off) { [LenovoEnergy]::SetConservation($false); Start-Sleep -Milliseconds 300 }
if ([LenovoEnergy]::IsConservationOn()) { "Conservation Mode: ON" } else { "Conservation Mode: OFF" }
