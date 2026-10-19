import java.io.*;
import java.nio.file.*;
import java.nio.file.attribute.*;
import java.util.*;
import java.util.zip.*;

/** Generates a Minecraft-server-like folder for testing gpz (synthetic terrain, real Anvil/zlib/gzip framing). */
public class MCGen {
    static long hash(long x) { x ^= x >>> 33; x *= 0xff51afd7ed558ccdL; x ^= x >>> 33; x *= 0xc4ceb9fe1a85ec53L; x ^= x >>> 33; return x; }
    static double noise(long seed, int x, int z) {
        double v = 0; double amp = 1, fr = 0.01;
        for (int o = 0; o < 4; o++) { v += amp * (Math.sin(x * fr + seed % 7 + o) * Math.cos(z * fr * 1.3 + o * 2) + 0.4 * Math.sin((x + z) * fr * 2.1)); amp *= 0.5; fr *= 2; }
        return v;
    }
    static void tag(DataOutputStream o, int t, String n) throws IOException { o.writeByte(t); o.writeUTF(n); }

    static byte[] chunk(long seed, int cx, int cz, int nsec) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream(); DataOutputStream o = new DataOutputStream(bo);
        int[] h = new int[256];
        for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++)
            h[z * 16 + x] = 64 + (int) (18 * noise(seed, cx * 16 + x, cz * 16 + z)) + (int) (hash(seed + (cx * 16 + x) * 31L + (cz * 16 + z)) & 1);
        tag(o, 10, "");
        tag(o, 3, "DataVersion"); o.writeInt(3465);
        tag(o, 3, "xPos"); o.writeInt(cx); tag(o, 3, "zPos"); o.writeInt(cz); tag(o, 3, "yPos"); o.writeInt(-4);
        tag(o, 8, "Status"); o.writeUTF("minecraft:full");
        tag(o, 4, "LastUpdate"); o.writeLong(123456789L + cx); tag(o, 4, "InhabitedTime"); o.writeLong(hash(cx * 7 + cz) & 0xFFFFF);
        tag(o, 9, "sections"); o.writeByte(10); o.writeInt(nsec);
        for (int s = 0; s < nsec; s++) {
            int y0 = (s - 4) * 16;
            int[] idx = new int[4096]; ArrayList<String> pal = new ArrayList<>(); HashMap<String, Integer> pm = new HashMap<>();
            for (int y = 0; y < 16; y++) for (int z = 0; z < 16; z++) for (int x = 0; x < 16; x++) {
                int wy = y0 + y, hh = h[z * 16 + x]; String b;
                long r = hash(seed ^ ((long) (cx * 16 + x) * 73856093L) ^ ((long) wy * 19349663L) ^ ((long) (cz * 16 + z) * 83492791L));
                if (wy < -60) b = wy == -64 ? "minecraft:bedrock" : ((r & 3) == 0 ? "minecraft:bedrock" : "minecraft:deepslate");
                else if (wy > hh) b = wy <= 62 ? "minecraft:water" : "minecraft:air";
                else if (wy == hh) b = hh < 62 ? "minecraft:sand" : "minecraft:grass_block";
                else if (wy >= hh - 4) b = hh < 62 ? "minecraft:sand" : "minecraft:dirt";
                else {
                    b = wy < 0 ? "minecraft:deepslate" : "minecraft:stone";
                    long k = r & 0xFFFF;
                    if (k < 600) b = "minecraft:coal_ore"; else if (k < 800) b = "minecraft:iron_ore"; else if (k < 830 && wy < 16) b = "minecraft:diamond_ore";
                    else if (k < 900 && wy < 32) b = "minecraft:gold_ore"; else if (k < 1100) b = "minecraft:gravel";
                    if (((r >>> 20) & 0xFF) < 7) b = "minecraft:air";   // caves
                }
                Integer id = pm.get(b); if (id == null) { id = pal.size(); pal.add(b); pm.put(b, id); }
                idx[y * 256 + z * 16 + x] = id;
            }
            o.writeByte(1); o.writeUTF("Y"); // placeholder fixed below
            o.writeByte((byte) (s - 4));
            tag(o, 10, "block_states");
            tag(o, 9, "palette"); o.writeByte(10); o.writeInt(pal.size());
            for (String p : pal) { tag(o, 8, "Name"); o.writeUTF(p); o.writeByte(0); }
            if (pal.size() > 1) {
                int bits = Math.max(4, 32 - Integer.numberOfLeadingZeros(pal.size() - 1));
                int per = 64 / bits, n = (4096 + per - 1) / per;
                tag(o, 12, "data"); o.writeInt(n);
                for (int i = 0; i < n; i++) { long v = 0; for (int j = 0; j < per && i * per + j < 4096; j++) v |= ((long) idx[i * per + j]) << (j * bits); o.writeLong(v); }
            }
            o.writeByte(0);
            tag(o, 10, "biomes"); tag(o, 9, "palette"); o.writeByte(8); o.writeInt(1); o.writeUTF(wy0biome(y0, seed, cx, cz)); o.writeByte(0);
            byte[] sky = new byte[2048], blk = new byte[2048];
            for (int i = 0; i < 2048; i++) { int wy = y0 + ((i * 2) >> 8); int hh = h[(((i * 2) >> 4) & 15) * 16 + ((i * 2) & 15)]; int lv = wy > hh ? 15 : Math.max(0, 15 - (hh - wy)); sky[i] = (byte) (lv | (lv << 4)); }
            tag(o, 7, "SkyLight"); o.writeInt(2048); o.write(sky);
            if (y0 < 40 && (hash(seed + s * 1000L + cx * 31L + cz) & 3) == 0) { for (int i = 0; i < 2048; i++) blk[i] = (byte) (hash(i + cx + cz) & 0x11); tag(o, 7, "BlockLight"); o.writeInt(2048); o.write(blk); }
            o.writeByte(0);
        }
        tag(o, 10, "Heightmaps");
        for (String nme : new String[] {"MOTION_BLOCKING", "OCEAN_FLOOR", "WORLD_SURFACE", "WORLD_SURFACE_WG"}) {
            tag(o, 12, nme); o.writeInt(37);
            for (int i = 0; i < 37; i++) { long v = 0; for (int j = 0; j < 7 && i * 7 + j < 256; j++) v |= ((long) (h[i * 7 + j] + 64)) << (j * 9); o.writeLong(v); }
        }
        o.writeByte(0);
        tag(o, 10, "structures"); tag(o, 10, "References"); o.writeByte(0); tag(o, 10, "starts"); o.writeByte(0); o.writeByte(0);
        tag(o, 9, "block_entities"); o.writeByte(0); o.writeInt(0);
        tag(o, 9, "PostProcessing"); o.writeByte(0); o.writeInt(0);
        o.writeByte(0);
        return bo.toByteArray();
    }
    static String wy0biome(int y, long seed, int cx, int cz) { return (hash(seed + cx / 4 * 31 + cz / 4) & 3) == 0 ? "minecraft:forest" : "minecraft:plains"; }

    static void region(Path f, long seed, int rx, int rz, int w, int hgt, int nsec) throws IOException {
        Files.createDirectories(f.getParent());
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        int[] loc = new int[1024], ts = new int[1024]; int sector = 2;
        for (int cz = 0; cz < hgt; cz++) for (int cx = 0; cx < w; cx++) {
            byte[] raw = chunk(seed, rx * 32 + cx, rz * 32 + cz, nsec);
            ByteArrayOutputStream cb = new ByteArrayOutputStream();
            try (DeflaterOutputStream d = new DeflaterOutputStream(new BufferedOutputStream(cb, 8192))) { d.write(raw); }   // as Minecraft does
            byte[] comp = cb.toByteArray();
            int len = comp.length + 1, total = 4 + len, secs = (total + 4095) / 4096;
            DataOutputStream bo = new DataOutputStream(body);
            bo.writeInt(len); bo.writeByte(2); bo.write(comp);
            byte[] pad = new byte[secs * 4096 - total]; bo.write(pad);
            loc[cz * 32 + cx] = (sector << 8) | secs; ts[cz * 32 + cx] = 1700000000 + cx * 60 + cz;
            sector += secs;
        }
        try (DataOutputStream o = new DataOutputStream(new FileOutputStream(f.toFile()))) {
            for (int v : loc) o.writeInt(v); for (int v : ts) o.writeInt(v); body.writeTo(o);
        }
    }
    static void gz(Path f, byte[] data) throws IOException { Files.createDirectories(f.getParent()); try (GZIPOutputStream g = new GZIPOutputStream(new FileOutputStream(f.toFile()))) { g.write(data); } }
    static void text(Path f, String s) throws IOException { Files.createDirectories(f.getParent()); Files.write(f, s.getBytes("UTF-8")); }

    public static void main(String[] a) throws Exception {
        Path r = Paths.get(a[0]); int scale = a.length > 1 ? Integer.parseInt(a[1]) : 1;   // 1 = tiny (tests), 4 = larger
        long seed = 12345; int d = 4 * scale, nsec = scale == 1 ? 6 : 12;
        region(r.resolve("world/region/r.0.0.mca"), seed, 0, 0, 4 * d, 4 * d, nsec);
        region(r.resolve("world/region/r.-1.0.mca"), seed, -1, 0, 2 * d, 2 * d, nsec);
        region(r.resolve("world/region/r.0.-1.mca"), seed, 0, -1, 2 * d, d, nsec);
        region(r.resolve("world/DIM-1/region/r.0.0.mca"), seed + 1, 0, 0, 2 * d, 2 * d, nsec);
        region(r.resolve("world/DIM1/region/r.0.0.mca"), seed + 2, 0, 0, d, d, nsec);
        region(r.resolve("world/entities/r.0.0.mca"), seed + 3, 0, 0, d, d, 1);
        // level.dat / playerdata (gzip NBT)
        ByteArrayOutputStream bo = new ByteArrayOutputStream(); DataOutputStream o = new DataOutputStream(bo);
        tag(o, 10, ""); tag(o, 10, "Data"); tag(o, 8, "LevelName"); o.writeUTF("world"); tag(o, 3, "version"); o.writeInt(19133);
        tag(o, 4, "RandomSeed"); o.writeLong(seed); tag(o, 3, "SpawnX"); o.writeInt(0); tag(o, 3, "SpawnZ"); o.writeInt(0); o.writeByte(0); o.writeByte(0);
        gz(r.resolve("world/level.dat"), bo.toByteArray());
        for (int p = 0; p < 3 * scale; p++) {
            bo = new ByteArrayOutputStream(); o = new DataOutputStream(bo);
            tag(o, 10, ""); tag(o, 9, "Pos"); o.writeByte(6); o.writeInt(3); for (int k = 0; k < 3; k++) o.writeDouble(hash(p * 3 + k) % 1000 / 3.0);
            tag(o, 9, "Inventory"); o.writeByte(10); o.writeInt(36); for (int k = 0; k < 36; k++) { tag(o, 8, "id"); o.writeUTF("minecraft:stone"); tag(o, 1, "Count"); o.writeByte(k + 1); tag(o, 1, "Slot"); o.writeByte(k); o.writeByte(0); }
            o.writeByte(0);
            gz(r.resolve(String.format("world/playerdata/00000000-0000-0000-0000-%012d.dat", p)), bo.toByteArray());
        }
        // logs
        StringBuilder log = new StringBuilder(); Random rnd = new Random(7);
        String[] msgs = {"Player%d joined the game", "<Player%d> hello everyone", "Player%d left the game", "Saving the game (this may take a moment!)", "Can't keep up! Is the server overloaded? Running %dms behind", "Player%d was slain by Zombie"};
        for (int i = 0; i < 6000 * scale; i++) log.append(String.format("[%02d:%02d:%02d] [Server thread/INFO]: ", i / 3600 % 24, i / 60 % 60, i % 60)).append(String.format(msgs[rnd.nextInt(msgs.length)], rnd.nextInt(20))).append('\n');
        text(r.resolve("logs/latest.log"), log.toString());
        gz(r.resolve("logs/2024-01-01-1.log.gz"), log.substring(0, log.length() / 2).getBytes("UTF-8"));
        // plugin jar
        Files.createDirectories(r.resolve("plugins"));
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(r.resolve("plugins/Example.jar").toFile()))) {
            for (int i = 0; i < 20; i++) { z.putNextEntry(new ZipEntry("com/example/Class" + i + ".class")); byte[] b = new byte[3000]; for (int k = 0; k < b.length; k++) b[k] = (byte) (k % 17 == 0 ? hash(i * 5000 + k) : k / 11 + i); z.write(b); z.closeEntry(); }
            z.putNextEntry(new ZipEntry("plugin.yml")); z.write("name: Example\nversion: 1.0\nmain: com.example.Main\n".getBytes()); z.closeEntry();
        }
        text(r.resolve("plugins/Example/config.yml"), "# settings\nenabled: true\nmessage: \"Welcome to the server\"\nlimits:\n  a: 1\n  b: 2\n");
        Files.createDirectories(r.resolve("plugins/empty"));
        text(r.resolve("server.properties"), "#Minecraft server properties\nmotd=A Minecraft Server\nlevel-name=world\nserver-port=25565\nmax-players=20\nonline-mode=true\n");
        text(r.resolve("eula.txt"), "eula=true\n"); text(r.resolve("ops.json"), "[]\n"); text(r.resolve("usercache.json"), "[]\n");
        text(r.resolve("world/session.lock"), "☃");
        text(r.resolve("start.sh"), "#!/bin/sh\nexec java -Xmx2G -jar server.jar nogui\n");
        Files.setPosixFilePermissions(r.resolve("start.sh"), PosixFilePermissions.fromString("rwxr-xr-x"));
        text(r.resolve("secret.txt"), "private\n"); Files.setPosixFilePermissions(r.resolve("secret.txt"), PosixFilePermissions.fromString("rw-------"));
        Files.createSymbolicLink(r.resolve("current"), Paths.get("world"));
        text(r.resolve("world/data/empty.dat"), ""); 
        // fixed, old timestamps so mtime preservation is testable
        Files.walk(r).forEach(p -> { try { if (!Files.isSymbolicLink(p)) Files.setLastModifiedTime(p, FileTime.fromMillis(1700000000000L + (p.toString().hashCode() & 0xFFFFF) * 1000L)); } catch (IOException e) { } });
    }
}
