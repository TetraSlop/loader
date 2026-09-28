"""Fresh client configuration; deployment addresses contain no server code."""

from ipaddress import IPv4Address


def config_text(
    upgraded=False, server_address="127.0.0.1", game_port=51241, profile_port=51220
):
    address = str(IPv4Address(server_address))
    if not all(1 <= port <= 65535 for port in (game_port, profile_port)):
        raise ValueError("Service ports must be in 1..65535")
    if game_port == profile_port or profile_port == 51241:
        raise ValueError(
            "Service ports must differ; profile cannot use reserved game port51241"
        )
    return (
        "# Connect to a separately deployed server. No server is started by this app.\n"
        "# Replace the example account with credentials supplied by your server operator.\n"
        "runtime-dir=runtime\ngame=TM_native.dll\ncore=polcore_native.dll\n"
        f"log=logs/tmloader.log\nserver={address}\ngame-port={game_port}\n"
        f"profile-port={profile_port}\npol-id=EEEEEEEF\npassword=tm-local-test\n"
        "diagnostics=false\ntexture-pack="
        + ("textures-hd/pack.json" if upgraded else "textures/pack.json")
        + "\n"
    )
