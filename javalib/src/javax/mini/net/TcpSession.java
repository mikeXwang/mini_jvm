/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */
package javax.mini.net;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import javax.mini.net.Socket;
import java.util.LinkedList;

/**
 *
 * @author gust
 */
public class TcpSession {

    Socket conn;
    LinkedList rpool = new LinkedList();
    LinkedList spool = new LinkedList();

    /**
     *
     */
    public TcpSession(Socket sock) {
        this.conn = sock;
    }

    public void action() throws IOException {
        rcvAndDecode();
        sndAndEncode();
    }

    public void send(byte[] data) {
        synchronized (spool) {
            spool.add(data);
        }
    }

    public byte[] receive() {
        synchronized (rpool) {
            if (!rpool.isEmpty()) {
                return (byte[]) rpool.removeFirst();
            }
        }
        return null;
    }
    /**
     * 接收数据
     */
    static final int PKG_BYTESSIZE_LEN = 4;// 包 长度定义 为几个字节
    byte[] tmpRbuf = new byte[256];

    int lengthNeed = 0;
    byte[] lengthBuf = new byte[4];
    int rcvNeed = 0;
    ByteArrayOutputStream rcvBuf = new ByteArrayOutputStream();

    void rcvAndDecode() throws IOException {

        while (true) {// 循环接收多个数据包
            if (rcvNeed == 0) {
                if (lengthNeed == 0) {
                    lengthNeed = PKG_BYTESSIZE_LEN;
                }
                // 取长度
                int r = conn.read(lengthBuf, PKG_BYTESSIZE_LEN - lengthNeed, lengthNeed);
                if (r < 0) {
                    throw new IOException("read pkg len error");
                } else if (r == 0) {
                    return;
                } else {
                    lengthNeed -= r;
                    if (lengthNeed == 0) {
                        rcvNeed = ((lengthBuf[0] & 0xff) << 24) | ((lengthBuf[1] & 0xff) << 16) | ((lengthBuf[2] & 0xff) << 8) | (lengthBuf[3] & 0xff);
                        rcvBuf.reset();
                        rcvBuf.write(lengthBuf);
                    }
                }
            }
            if (rcvNeed > 0) {
                int copy = rcvNeed < tmpRbuf.length ? rcvNeed : tmpRbuf.length;
                int r = conn.read(tmpRbuf, 0, copy);
                if (r < 0) {
                    throw new IOException("read pkg error");
                } else if (r == 0) {
                    return;
                } else {
                    rcvBuf.write(tmpRbuf, 0, r);
                    rcvNeed -= r;
                    if (rcvNeed == 0) {
                        byte[] b = rcvBuf.toByteArray();
                        synchronized (rpool) {
                            rpool.add(b);
                        }
                    }
                }
            }
        }
    }

    /**
     * 发送
     */
    int sent;
    private byte[] sndBuf;

    void sndAndEncode() throws IOException {

        while (true) {// 循环接收多个数据包
            if (sndBuf == null) {
                synchronized (spool) {
                    if (!spool.isEmpty()) {
                        sndBuf = (byte[]) spool.removeFirst();
                    } else {
                        break;
                    }
                }
            }
            if (sndBuf != null) {
                int w = conn.write(sndBuf, sent, sndBuf.length - sent);
                if (w < 0) {
                    throw new IOException("write data error.");
                } else if (w == 0) {
                    break;
                } else {
                    sent += w;
                    if (sent == sndBuf.length) {
                        sndBuf = null;
                        sent = 0;
                    }
                }

            }

        }
    }

    // ----------------------------------------------------------------------------
    // no comment
    // ----------------------------------------------------------------------------
    static public void print(byte[] b) {
        for (int i = 0; i < b.length; i++) {
            String s = Integer.toHexString(b[i]);
            s = "00" + s;
            String tgt = "";
            for (int j = 0; j < 2; j++) {
                tgt = (s.charAt(s.length() - 1 - j)) + tgt;
            }
            System.out.print(" " + tgt);
        }
        System.out.println();
    }

}
