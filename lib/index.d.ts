export default class FasterSerialPort {
    isOpen: boolean;
  
    constructor(path: string, options: Partial<{
      autoOpen: boolean;
      endOnClose: boolean;
      baudRate: number;
      dataBits: number;
      hupcl: boolean;
      lock: boolean;
      parity: "none";
      rtscts: boolean;
      stopBits: number;
      xany: boolean;
      xoff: boolean;
      xon: boolean;
      eventsCallback: (err: string | null, arg: {event?: number, errorCode?: number}) => void;
    }>);
  
    write(buf: Buffer | number[], echoMode: boolean = false): Promise<void>;
    read(nBytes: number): Promise<Buffer>;
    bufferedRead(callback: (data: Buffer) => boolean, dataGapMs?: number): Promise<void>;
    bufferedReadExt(callback: (data: Buffer) => boolean, opts?: {
      idleAllowanceMs?: number;
      noDataTimeoutMs?: number;
      pollTimeoutMs?: number;
      batchSize?: number;
    }): Promise<void>;
    setTimeout(ms: number): void;
    close(): Promise<void>;
    open(): Promise<void>;
    flush(): Promise<void>;
    drain(): Promise<void>;
    update(opts: {
      baudRate:
        | 115200
        | 57600
        | 38400
        | 19200
        | 9600
        | 4800
        | 2400
        | 1800
        | 1200
        | 600
        | 300
        | 200
        | 150
        | 134
        | 110
        | 75
        | 50
        | number;
    }): Promise<void>;
  
    static list(): Promise<PortInfo[]>;
    static configureLogging(enabled: boolean, dir: string): void;
  }

  export interface PortInfo {
    path: string;
    manufacturer?: string;
    serialNumber?: string;
    pnpId?: string;
    locationId?: string;
    productId?: string;
    vendorId?: string;
  }