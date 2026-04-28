# Launch Demo

The launch recording is stored as a static GIF at
`docs/assets/node-vmm-launch.gif`. The source tape/scripts used to create the
GIF are intentionally not tracked in this repository or published to npm. Keep
local regeneration sources under `.node-vmm-demo/`, which is ignored by Git.

The commands shown in the tape are the intended first-run flow:

1. Install `node-vmm`.
2. Inspect host/runtime features.
3. Show a real Git repository with `app/Dockerfile`, `app/package.json`, and
   `app/app.js`.
4. Fetch the guest kernel.
5. Build a Node rootfs from that Git repository.
6. Start the VM in interactive console mode.
7. Run `node /workspace/app.js` inside the guest.

Real KVM app execution still requires Linux, `/dev/kvm`, and root privileges for
rootfs mount/network setup.
