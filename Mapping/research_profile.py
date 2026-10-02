"""Sample process-local Metal allocations without disabling allocator limits."""
import resource
import sys
import threading


class MemorySampler:
    def __init__(self, torch, device, interval=.01):
        self.torch, self.device, self.interval = torch, device, interval
        self.peak_driver = self.peak_tensor = self.samples = 0
        self.error = None
        self.stop_event = threading.Event()
        self.thread = None

    def sample(self):
        if self.device != 'mps': return
        try:
            self.peak_driver = max(self.peak_driver, self.torch.mps.driver_allocated_memory())
            self.peak_tensor = max(self.peak_tensor, self.torch.mps.current_allocated_memory())
            self.samples += 1
        except Exception as exc:
            self.error = str(exc)

    def start(self):
        self.sample()
        def loop():
            while not self.stop_event.wait(self.interval): self.sample()
        self.thread = threading.Thread(target=loop, daemon=True)
        self.thread.start()

    def finish(self):
        self.stop_event.set()
        if self.thread: self.thread.join()
        self.sample()
        rss = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        return dict(sampled_mps_driver_peak_bytes=self.peak_driver if self.device == 'mps' else None,
                    sampled_mps_tensor_peak_bytes=self.peak_tensor if self.device == 'mps' else None,
                    process_peak_rss_bytes=rss if sys.platform == 'darwin' else rss*1024,
                    sample_interval_seconds=self.interval, samples=self.samples, sampling_error=self.error,
                    note='Sampled Metal peak may miss brief allocations. Driver includes caches; RSS and Metal overlap and must not be added. Other apps are excluded.')
