# Print addresses friends can use to join this machine's playtest server.
Write-Host "=== PaintForge host addresses ==="
Write-Host ""

Write-Host "IPv4 adapters (prefer Ethernet/Wi-Fi or VPN):"
Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
	Where-Object { $_.IPAddress -notlike "127.*" -and $_.PrefixOrigin -ne "WellKnown" } |
	Sort-Object InterfaceAlias |
	ForEach-Object {
		$Alias = $_.InterfaceAlias
		$Ip = $_.IPAddress
		$Kind = if ($Alias -match "Tailscale|ZeroTier|WireGuard|VPN|Hamachi|NordLynx") { "VPN " } else { "LAN " }
		Write-Host ("  [{0}] {1,-28}  {2}:7777" -f $Kind, $Alias, $Ip)
	}

Write-Host ""
Write-Host "Friends run (packaged client console ~ ):"
Write-Host "  open <ip-above>:7777"
Write-Host "Or:  .\connect.ps1 -Server <ip-above>"
