/**
 * The Host's serial transport to the Bridge. Roadmap T1.5.
 *
 * Frames in, frames out; SLIP is an implementation detail below this line and
 * the caller never sees a delimiter.
 *
 * The class takes a byte stream rather than a port path, and that is the whole
 * design. A transport that opens its own hardware can only be tested with
 * hardware attached, which in practice means it is tested rarely and by hand.
 * With the stream injected, the framing, the error accounting and the leak
 * behaviour are all exercised in ordinary unit tests, and `openSerialTransport`
 * is left as the thin adapter that knows about `serialport`.
 */
import { SlipDecoder, slipEncode, type SlipStats } from './slip.js';

/**
 * The part of a Node duplex stream this transport uses.
 *
 * Structural rather than `import type { SerialPort }`: a mock in a test, a
 * socket, or a pair of pipes are all legitimate, and nothing here needs to know
 * which it has.
 */
export interface ByteStream {
  on(event: 'data', listener: (chunk: Buffer) => void): unknown;
  on(event: 'error', listener: (error: Error) => void): unknown;
  on(event: 'close', listener: () => void): unknown;
  write(data: Uint8Array): boolean;
}

export type FrameListener = (frame: Buffer) => void;
export type ErrorListener = (error: Error) => void;

export interface SerialTransportOptions {
  /**
   * Largest frame accepted. Defaults to the Bridge's own receive buffer — a
   * frame the Bridge could not have sent is one we need not be able to receive,
   * and a decoder sized generously is a decoder that will happily buffer
   * garbage forever when the line is noisy.
   */
  maxFrameBytes?: number;
}

export interface SerialTransportStats extends SlipStats {
  framesSent: number;
  bytesSent: number;
  bytesReceived: number;
  /** Frames that could not be handed to the OS because the stream refused. */
  writeFailures: number;
}

export class SerialTransport {
  private readonly decoder: SlipDecoder;
  private readonly frameListeners: FrameListener[] = [];
  private readonly errorListeners: ErrorListener[] = [];

  private framesSent = 0;
  private bytesSent = 0;
  private bytesReceived = 0;
  private writeFailures = 0;
  private closed = false;

  constructor(
    private readonly stream: ByteStream,
    options: SerialTransportOptions = {},
  ) {
    this.decoder = new SlipDecoder(options.maxFrameBytes ?? 256);

    this.stream.on('data', (chunk) => this.handleChunk(chunk));
    this.stream.on('error', (error) => {
      for (const listener of this.errorListeners) listener(error);
    });
    this.stream.on('close', () => {
      this.closed = true;
    });
  }

  private handleChunk(chunk: Buffer): void {
    this.bytesReceived += chunk.length;
    for (const frame of this.decoder.push(chunk)) {
      for (const listener of this.frameListeners) listener(frame);
    }
  }

  onFrame(listener: FrameListener): void {
    this.frameListeners.push(listener);
  }

  onError(listener: ErrorListener): void {
    this.errorListeners.push(listener);
  }

  /**
   * Frames and writes one message. Returns false if the port is closed or the
   * stream refused it.
   *
   * No validation of the frame's contents happens here. The Bridge validates
   * everything before it spends airtime on it (ARCHITECTURE.md §7.4), and a
   * transport that silently dropped malformed frames would hide exactly the
   * bugs this layer should be making visible.
   */
  send(frame: Uint8Array): boolean {
    if (this.closed) return false;

    const encoded = slipEncode(frame);
    if (!this.stream.write(encoded)) {
      // `write` returning false is normally backpressure rather than failure,
      // and the data is still queued. Counted, not treated as a loss: a sender
      // that retried here would duplicate the frame.
      this.writeFailures++;
    }

    this.framesSent++;
    this.bytesSent += encoded.length;
    return true;
  }

  /** Framing state is discarded, counters are kept — for a reopened port. */
  resetFraming(): void {
    this.decoder.reset();
  }

  get stats(): SerialTransportStats {
    return {
      ...this.decoder.stats,
      framesSent: this.framesSent,
      bytesSent: this.bytesSent,
      bytesReceived: this.bytesReceived,
      writeFailures: this.writeFailures,
    };
  }
}

export interface OpenSerialOptions extends SerialTransportOptions {
  /** e.g. `COM4` on Windows, `/dev/ttyUSB0` on Linux. */
  path: string;
  /** Must match the Bridge's `SerialLink::kDefaultBaud`. */
  baudRate?: number;
}

/**
 * Opens a real port and wraps it.
 *
 * `serialport` is imported dynamically so that everything above — the codec,
 * the transport, the tests — loads on a machine with no native bindings built,
 * and so that a CI job that never touches hardware never pays for them.
 */
export async function openSerialTransport(options: OpenSerialOptions): Promise<SerialTransport> {
  const { SerialPort } = await import('serialport');

  const port = new SerialPort({
    path: options.path,
    // 921600 to match the Bridge. A mismatch here does not error — it delivers
    // plausible-looking garbage, which SLIP then rejects as bad escapes, so
    // `stats.badEscape` climbing on a fresh link is the first thing to suspect.
    baudRate: options.baudRate ?? 921600,
    autoOpen: false,
  });

  await new Promise<void>((resolve, reject) => {
    port.open((error) => (error ? reject(error) : resolve()));
  });

  const transportOptions: SerialTransportOptions = {};
  if (options.maxFrameBytes !== undefined) transportOptions.maxFrameBytes = options.maxFrameBytes;

  return new SerialTransport(port, transportOptions);
}
