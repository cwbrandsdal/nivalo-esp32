from pathlib import Path

Import("env")


def patch_adafruit_dap_stm32(*_args, **_kwargs):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    env_name = env.subst("$PIOENV")
    source = (
        project_dir
        / ".pio"
        / "libdeps"
        / env_name
        / "Adafruit DAP library"
        / "Adafruit_DAP_STM32.cpp"
    )

    if not source.exists():
        print(f"Adafruit DAP STM32 patch skipped; {source} not found yet")
        return

    text = source.read_text()
    marker = '    {0x419, "STM32F42xxx and STM32F43xxx"},'
    addition = '    {0x421, "STM32F446xx/F469xx/F479xx"},'

    if addition in text:
        return

    if marker not in text:
        raise RuntimeError("Could not locate Adafruit DAP STM32 device table")

    source.write_text(text.replace(marker, marker + "\n" + addition))
    print("Patched Adafruit DAP STM32 device table for MCU ID 0x421")


patch_adafruit_dap_stm32()
env.AddPreAction("buildprog", patch_adafruit_dap_stm32)
