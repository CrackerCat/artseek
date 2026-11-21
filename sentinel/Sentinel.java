public class Sentinel {
    public int i1 = 0x11111111;
    public long l1 = 0x2222222222222222L;
    public String s1 = "ZZ_SENTINEL_ASCII";
    public int i2 = 0x33333333;
    public Sentinel next;

    public static int sMarker = 0x44445555;
    public static Object[] sKeep;

    public static class Sub extends Sentinel { public int k1 = 0x55556666; }
    public static final class Tiny { public int z1 = 0x0BADF00D; }

    public static void main(String[] args) throws Exception {
        Sentinel x = new Sentinel();
        x.next = x;
        Sub sub = new Sub();
        Tiny tiny = new Tiny();
        Sentinel[] bunch = new Sentinel[8];
        for (int i = 0; i < 8; i++) bunch[i] = new Sentinel();
        sKeep = new Object[] { x, sub, tiny, bunch };
        System.out.println("name=" + Sentinel.class.getName());
        System.out.println("ready " + sub.k1 + " " + tiny.z1 + " " + bunch[3].i2 + " " + sMarker);
        System.out.flush();
        while (true) Thread.sleep(1000);
    }
}
