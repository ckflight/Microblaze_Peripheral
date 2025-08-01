# 2025-08-01T15:08:40.768134400
import vitis

client = vitis.create_client()
client.set_workspace(path="Microblaze_Peripheral")

platform = client.get_component(name="Microblaze")
status = platform.build()

comp = client.get_component(name="Microblaze_Peripherals")
comp.build()

status = comp.clean()

status = platform.build()

status = platform.build()

comp.build()

status = platform.build()

comp.build()

