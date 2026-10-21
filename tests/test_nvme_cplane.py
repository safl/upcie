import pytest
from conftest import iommu_available, uio_devices

SOCKET = "/tmp/upcie_cplane_test.sock"


def cplane_session(cijoe, bdf, client_mode, use_vfio=False):
    """
    Run a server and a client against each other in two processes.

    The server serves exactly one client and exits, so the session is one shell
    command: start it, wait for the socket it listens on, run the client, and
    wait for the server to tear down. The session fails when either fails, and
    when the server is still running after the client; it is killed then, which
    also covers a client that failed before the server had anything to answer.
    """

    mode = " vfio" if use_vfio else ""

    return cijoe.run(
        f"rm -f {SOCKET};"
        f" setsid test_nvme_cplane server {bdf} {SOCKET}{mode}"
        " > /tmp/upcie_cplane_server.out 2>&1 < /dev/null & spid=$!;"
        f" for i in $(seq 1 100); do [ -S {SOCKET} ] && break; sleep 0.1; done;"
        f" test_nvme_cplane {client_mode} {SOCKET}; rc=$?;"
        " for i in $(seq 1 100); do kill -0 $spid 2>/dev/null || break; sleep 0.1; done;"
        " if kill -0 $spid 2>/dev/null; then kill $spid; rc=1; fi;"
        " wait $spid; src=$?; [ $rc -eq 0 ] && rc=$src;"
        " cat /tmp/upcie_cplane_server.out; exit $rc"
    )


@pytest.mark.parametrize("bdf", uio_devices())
def test_cplane_client_attaches_uio(cijoe, bdf):
    """A client attaches over the socket and drives a queue of its own."""

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cplane_session(cijoe, bdf, "client")
    assert not err


@pytest.mark.skipif(
    not iommu_available(), reason="guest exposes no IOMMU groups; vfio-pci unavailable"
)
@pytest.mark.parametrize("bdf", uio_devices())
def test_cplane_client_attaches_vfio(cijoe, bdf):
    """The same, behind an IOMMU.

    This is the case the arrangement exists for. A vfio device file cannot be
    bound twice, so a client can only reach the controller by being handed the
    descriptor over the socket; under uio_pci_generic it could have opened its
    own and the passing would prove less.
    """

    err, _ = cijoe.run("devbind --bind vfio-pci")
    assert not err

    err, _ = cplane_session(cijoe, bdf, "client", use_vfio=True)
    assert not err


@pytest.mark.parametrize("bdf", uio_devices())
def test_cplane_refuses_a_stranger_version(cijoe, bdf):
    """A peer speaking a version nobody knows is refused rather than served.

    The messages describe queue memory whose layout comes from this library,
    so a client built against another version cannot be trusted to read it. The
    client mode here sends a version nobody speaks and fails unless it is
    turned away, so a passing run means the refusal happened.
    """

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cplane_session(cijoe, bdf, "client-badversion")
    assert not err


@pytest.mark.parametrize("bdf", uio_devices())
def test_cplane_client_places_queues_uio(cijoe, bdf):
    """A client registers memory of its own and has queues placed in it.

    The client registers a udmabuf and reads LBA 0 twice: with the CQ in its
    memory and the SQ in the heap, and with both in its memory, so the server
    must translate the registration and the controller must use it. The test
    server describes a registration by pagemap, which is what uio needs.
    """

    err, _ = cijoe.run("test -c /dev/udmabuf")
    if err:
        pytest.skip("guest has no /dev/udmabuf to register memory with")

    err, _ = cijoe.run("devbind --bind uio_pci_generic")
    assert not err

    err, _ = cplane_session(cijoe, bdf, "client-at")
    assert not err
