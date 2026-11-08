import com.tyranor.runtime.systemnnn.SjisTunnelDecoder;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;
import java.util.Locale;
import java.util.stream.Collectors;

public final class TextCodecTest {
    private static byte[] bytes(int... values) { byte[] result = new byte[values.length]; for(int i=0;i<values.length;i++) result[i]=(byte)values[i]; return result; }
    private static void require(boolean value) { if(!value) throw new AssertionError(); }
    private interface Checked { void run() throws Exception; }
    private static void rejects(Checked run) throws Exception {
        try { run.run(); } catch(IOException expected) { return; }
        throw new AssertionError("Expected explicit rejection");
    }
    public static void main(String[] args) throws Exception {
        SjisTunnelDecoder decoder = new SjisTunnelDecoder(bytes(0x80,0x7b,0xd4,0x9a,0xa9,0x03));
        require(decoder.decode(bytes(65,0x81,1,0x81,2,0x81,3,90)).equals("A简體ΩZ"));
        require(decoder.decode(bytes(0x82,0xa0,0xa6,0x5c,0x7e)).equals("あｦ\\~"));
        require(decoder.decode(bytes()).isEmpty());
        rejects(()->decoder.decode(bytes(0x81)));
        rejects(()->decoder.decode(bytes(0x81,0)));
        rejects(()->decoder.decode(bytes(0x81,9)));
        rejects(()->decoder.decode(bytes(0x81,0x2c)));
        rejects(()->decoder.decode(bytes(0x81,4)));
        rejects(()->decoder.decode(bytes(0x81,0xff)));
        rejects(()->new SjisTunnelDecoder(bytes(1)));
        rejects(()->new SjisTunnelDecoder(bytes(0,0)));
        rejects(()->new SjisTunnelDecoder(bytes(0,0xd8)));
        rejects(()->new SjisTunnelDecoder(new byte[(SjisTunnelDecoder.MAX_MAPPINGS+1)*2]));
        rejects(()->decoder.decode(new byte[SjisTunnelDecoder.MAX_INPUT_BYTES+1]));
        byte[] largeMap = new byte[SjisTunnelDecoder.MAX_MAPPINGS*2];
        for(int i=0;i<SjisTunnelDecoder.MAX_MAPPINGS;i++) { int value=0x4e00+i; largeMap[i*2]=(byte)value; largeMap[i*2+1]=(byte)(value>>8); }
        SjisTunnelDecoder large = new SjisTunnelDecoder(largeMap);
        for(String line:Files.readAllLines(Paths.get(args[0]),StandardCharsets.UTF_8)) {
            String[] fields=line.split(" "); int index=Integer.parseInt(fields[0]), code=Integer.parseInt(fields[1]);
            require(large.decode(bytes(code>>8,code&255)).equals(Character.toString((char)(0x4e00+index))));
        }
        System.out.println("PASS fixed vectors, 11 rejection cases, all 3422 original producer pairs");
        for(String variant:Arrays.asList("zh","zht")) {
            Path root=Paths.get(args[1],"DeadEndAegis ["+variant+"]");
            SjisTunnelDecoder game=new SjisTunnelDecoder(Files.readAllBytes(root.resolve("sjis_ext.bin")));
            MessageDigest hash=MessageDigest.getInstance("SHA-256"); int count=0;
            List<Path> files;
            try(java.util.stream.Stream<Path> stream=Files.list(root.resolve("spt"))) {
                files=stream.filter(p->p.toString().endsWith(".spt")).sorted(Comparator.comparing(p->p.getFileName().toString().toLowerCase(Locale.ROOT))).collect(Collectors.toList());
            }
            for(Path path:files) {
                byte[] data=Files.readAllBytes(path); for(int i=0;i<data.length;i++) data[i]^=(byte)255;
                ByteBuffer words=ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
                for(int field:new int[]{4,6}) {
                    int strings=words.getInt(field*4), pointer=words.getInt((field+1)*4);
                    require(strings>=0 && pointer>=0 && ((long)pointer+strings)*4<=data.length);
                    for(int i=0;i<strings;i++) {
                        int start=words.getInt((pointer+i)*4)*4, end=start;
                        require(start>=0 && start<data.length);
                        while(end<data.length && data[end]!=0) end++;
                        require(end<data.length);
                        byte[] utf8=game.decode(Arrays.copyOfRange(data,start,end)).getBytes(StandardCharsets.UTF_8);
                        hash.update(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(utf8.length).array()); hash.update(utf8); count++;
                    }
                }
            }
            StringBuilder hex=new StringBuilder(); for(byte value:hash.digest()) hex.append(String.format("%02x",value&255));
            System.out.println(variant+" strings="+count+" utf8_sha256="+hex);
        }
    }
}
