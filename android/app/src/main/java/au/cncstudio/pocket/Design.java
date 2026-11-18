package au.cncstudio.pocket;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.Locale;

final class Design {
    static final String[] HEADS = {"router", "laser", "knife"};
    final ArrayList<Part> parts = new ArrayList<>();
    int selected = -1, head = 0;

    JSONObject draft() throws JSONException {
        JSONArray items = new JSONArray();
        for (Part p : parts) items.put(new JSONArray().put(p.type).put(p.sides)
                .put(p.x).put(p.y).put(p.width).put(p.height));
        return new JSONObject().put("parts",items).put("head",head).put("selected",selected);
    }

    static Design restore(String stored) throws JSONException {
        JSONObject draft = new JSONObject(stored);
        Design design = new Design();
        JSONArray items = draft.getJSONArray("parts");
        if (items.length()>128) throw new JSONException("Too many shapes");
        for(int i=0; i<items.length(); i++) {
            JSONArray p=items.getJSONArray(i);
            design.parts.add(new Part(p.getInt(0),p.getInt(1),p.getDouble(2),p.getDouble(3),p.getDouble(4),p.getDouble(5)));
        }
        design.head = Math.max(0,Math.min(2,draft.optInt("head")));
        design.selected = Math.max(-1,Math.min(design.parts.size()-1,draft.optInt("selected",-1)));
        return design;
    }

    String project() throws JSONException {
        JSONArray objects = new JSONArray();
        JSONArray paths = new JSONArray();
        for (Part p : parts) {
            JSONArray outline = new JSONArray();
            for (double[] pt : p.outline()) outline.put(new JSONArray().put(pt[0]).put(pt[1]));
            objects.put(new JSONObject().put("name",p.title()).put("paths",new JSONArray().put(outline)));
            paths.put(outline);
        }
        JSONObject settings = new JSONObject().put("width",680).put("height",240)
                .put("mode",HEADS[head]).put("operation","line").put("protocol","grbl")
                .put("depth",head == 2 ? .15 : 1).put("safe",5).put("step",.5)
                .put("feed",300).put("plunge",100).put("power",300).put("paths",paths);
        return new JSONObject().put("format","pi-cnc-studio/1").put("settings",settings)
                .put("metadata",new JSONObject().put("name","Pocket design")).put("objects",objects).toString(2);
    }

    String svg() {
        StringBuilder body = new StringBuilder("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"680mm\" height=\"240mm\" viewBox=\"0 0 680 240\"><title>Studio Pocket design</title>\n");
        for (Part p : parts) {
            body.append("<path fill=\"none\" stroke=\"#15443e\" stroke-width=\"0.25\" d=\"");
            double[][] outline = p.outline();
            for (int i=0; i<outline.length-1; i++)
                body.append(String.format(Locale.ROOT,"%s%.5f %.5f ", i==0 ? "M" : "L",outline[i][0],240-outline[i][1]));
            body.append("Z\"/>\n");
        }
        return body.append("</svg>\n").toString();
    }
}
