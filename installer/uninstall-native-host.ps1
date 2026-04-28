# uninstall-native-host.ps1
# Run by Inno Setup at uninstall time. Drops the HKCU native-messaging
# host registrations so the registered exe path doesn't outlive matata.
$keys = @(
    'HKCU:\Software\Google\Chrome\NativeMessagingHosts\com.matata.host',
    'HKCU:\Software\Microsoft\Edge\NativeMessagingHosts\com.matata.host',
    'HKCU:\Software\Chromium\NativeMessagingHosts\com.matata.host',
    'HKCU:\Software\BraveSoftware\Brave-Browser\NativeMessagingHosts\com.matata.host',
    'HKCU:\Software\Mozilla\NativeMessagingHosts\com.matata.host'
)
foreach ($k in $keys) {
    Remove-Item -LiteralPath $k -ErrorAction SilentlyContinue
}
