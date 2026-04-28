# Launch Demo

The launch recording is kept as a VHS tape so the repository can regenerate the
demo instead of storing a hand-edited terminal capture.

```bash
npm install
npm run build
npm run demo:vhs
```

The tape lives at `vhs/launch.tape` and writes
`docs/assets/node-vmm-launch.gif`.
The real command sequencing lives in `vhs/launch.expect`; it waits for the
actual build and VM output before moving to the next command.

The commands shown in the tape are the intended first-run flow:

1. Install `node-vmm`.
2. Fetch the guest kernel.
3. Inspect host/runtime features.
4. Build a Node rootfs from a Git repository.
5. Start the VM in interactive console mode.
6. Run `node -e "console.log(\"node-vmm\", 21 * 2)"` inside the guest.

Real KVM app execution still requires Linux, `/dev/kvm`, and root privileges for
rootfs mount/network setup.
