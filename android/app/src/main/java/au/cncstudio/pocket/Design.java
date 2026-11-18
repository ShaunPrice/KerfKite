package au.cncstudio.pocket;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;
import java.util.ArrayList;
import java.util.Locale;

final class Design {
    static final String[] HEADS={"router","laser","knife"};
    static final int MAX_BYTES=4_000_000;
    final ArrayList<Part> parts=new ArrayList<>();
    int selected=-1,head=0;
    double width=680,height=240;
    boolean artwork;
    String reference; // Complete desktop document kept for lossless re-export.

    void setBed(double width,double height) throws JSONException {
        if(!(width>=.2 && width<=100000 && height>=.2 && height<=100000))
            throw new JSONException("Invalid work area dimensions.");
        this.width=width;this.height=height;
    }

    JSONObject draft() throws JSONException {
        if(reference!=null)return new JSONObject().put("reference",reference).put("selected",selected);
        JSONArray items=new JSONArray();
        for(Part p:parts) items.put(p.type==Part.PATH ? new JSONObject().put("name",p.name).put("paths",p.jsonPaths()) : p.primitive());
        return new JSONObject().put("parts",items).put("head",head).put("selected",selected).put("width",width).put("height",height).put("artwork",artwork);
    }

    static Design restore(String stored) throws JSONException {
        JSONObject draft=new JSONObject(stored);Design design;
        if(draft.has("reference")){
            String source=draft.getString("reference");
            design=open(new JSONObject(source));design.reference=source;
            design.selected=Math.max(-1,Math.min(design.parts.size()-1,draft.optInt("selected",-1)));
            return design;
        }
        design=new Design();design.artwork=draft.optBoolean("artwork");
        design.setBed(draft.optDouble("width",680),draft.optDouble("height",240));
        JSONArray items=draft.getJSONArray("parts");int[] points={0};
        if(items.length()>128)throw new JSONException("Too many objects.");
        for(int i=0;i<items.length();i++){
            JSONObject obj=items.optJSONObject(i);
            Part p=obj==null?Part.primitive(items.getJSONArray(i)):Part.fromPaths(obj.getString("name"),obj.getJSONArray("paths"),points);
            p.checkBed(design.width,design.height);design.parts.add(p);
        }
        design.head=Math.max(0,Math.min(2,draft.optInt("head")));
        design.selected=Math.max(-1,Math.min(design.parts.size()-1,draft.optInt("selected",-1)));
        return design;
    }

    static Design open(JSONObject doc) throws JSONException {
        if(!"pi-cnc-studio/1".equals(doc.optString("format")))throw new JSONException("Choose a KerfKite Studio project (.cncstudio.json).");
        JSONArray items=doc.getJSONArray("objects");JSONObject settings=doc.getJSONObject("settings");
        if(items.length()==0||items.length()>128)throw new JSONException("Pocket opens projects with 1–128 vector objects.");
        Design result=new Design();result.setBed(settings.getDouble("width"),settings.getDouble("height"));
        String mode=settings.getString("mode");
        result.head=java.util.Arrays.asList(HEADS).indexOf(mode);
        if(result.head<0)throw new JSONException("Unsupported machine head.");
        int[] points={0};
        JSONObject info=doc.optJSONObject("metadata");result.artwork=info!=null && Boolean.TRUE.equals(info.opt("pocketArtwork"));
        for(int i=0;i<items.length();i++){
            JSONObject obj=items.getJSONObject(i);
            Part path=Part.fromPaths(obj.getString("name"),obj.getJSONArray("paths"),points);
            path.checkBed(result.width,result.height);
            Part shape=null;
            JSONArray metadata=obj.optJSONArray("pocketPrimitive");
            if(metadata!=null)shape=Part.primitive(metadata);
            // Older Pocket files contain sampled geometry only. Recover a primitive
            // only when both its label and every actual coordinate match.
            else if(path.name.equals("Rectangle")||path.name.equals("Ellipse")||path.name.matches("[0-9]{1,2}-sided polygon")){
                int type=path.name.equals("Rectangle")?0:path.name.equals("Ellipse")?1:2;
                try{shape=new Part(type,type==2?Integer.parseInt(path.name.substring(0,path.name.indexOf('-'))):6,path.x,path.y,path.width,path.height);}
                catch(IllegalArgumentException ignored){shape=null;}
            }
            result.parts.add(shape!=null && shape.name.equals(path.name) && shape.sameOutline(path)?shape:path);
        }
        result.selected=0;return result;
    }

    static Design importProject(String source,JSONObject doc,String name) throws JSONException {
        Design result=open(doc);
        JSONObject nativeDoc=result.portable(name);
        JSONArray actual=doc.getJSONArray("objects"), expected=nativeDoc.getJSONArray("objects");
        // Legacy Pocket did not save primitive parameters. Its outlines have
        // already been checked against regenerated parameters by open().
        for(int i=0;i<actual.length();i++)
            if(!actual.getJSONObject(i).has("pocketPrimitive"))expected.getJSONObject(i).remove("pocketPrimitive");
        if(!equivalent(doc,nativeDoc))result.reference=source;
        return result;
    }

    private static boolean equivalent(Object a,Object b) throws JSONException {
        if(a instanceof JSONObject && b instanceof JSONObject){
            JSONObject x=(JSONObject)a,y=(JSONObject)b;
            if(x.length()!=y.length())return false;
            java.util.Iterator<String> keys=x.keys();
            while(keys.hasNext()){
                String key=keys.next();
                if(!y.has(key)||!equivalent(x.get(key),y.get(key)))return false;
            }
            return true;
        }
        if(a instanceof JSONArray && b instanceof JSONArray){
            JSONArray x=(JSONArray)a,y=(JSONArray)b;
            if(x.length()!=y.length())return false;
            for(int i=0;i<x.length();i++)if(!equivalent(x.get(i),y.get(i)))return false;
            return true;
        }
        if(a instanceof Number && b instanceof Number)return ((Number)a).doubleValue()==((Number)b).doubleValue();
        return a.equals(b);
    }

    Design artworkCopy(){
        Design copy=new Design();copy.width=width;copy.height=height;copy.head=head;copy.artwork=true;
        // Parts are immutable; any edit replaces the selected Part.
        copy.parts.addAll(parts);copy.selected=selected;
        return copy;
    }

    String project(String name) throws JSONException {
        if(reference!=null)return reference;
        String payload=portable(name).toString();
        if(payload.getBytes(java.nio.charset.StandardCharsets.UTF_8).length>MAX_BYTES)
            throw new JSONException("Project exceeds the desktop 4 MB limit. Split the artwork before exporting.");
        return payload;
    }

    private JSONObject portable(String name) throws JSONException {
        JSONArray objects=new JSONArray(),paths=new JSONArray();
        for(Part p:parts){
            JSONArray lines=p.jsonPaths();JSONObject obj=new JSONObject().put("name",p.name).put("paths",lines);
            if(p.type!=Part.PATH)obj.put("pocketPrimitive",p.primitive());
            if(artwork)obj.put("output",false);
            objects.put(obj);
            if(!artwork)for(int i=0;i<lines.length();i++)paths.put(lines.get(i));
        }
        JSONObject settings=new JSONObject().put("width",width).put("height",height)
            .put("mode",HEADS[head]).put("operation","line").put("protocol","grbl")
            .put("depth",head==2?.15:1).put("safe",5).put("step",.5)
            .put("feed",300).put("plunge",100).put("power",300).put("paths",paths);
        JSONObject metadata=new JSONObject().put("name",name);
        if(artwork)metadata.put("pocketArtwork",true);
        return new JSONObject().put("format","pi-cnc-studio/1").put("settings",settings)
            .put("metadata",metadata).put("objects",objects);
    }

    String svg(){
        StringBuilder body=new StringBuilder("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"").append(width)
            .append("mm\" height=\"").append(height).append("mm\" viewBox=\"0 0 ").append(width).append(' ').append(height)
            .append("\"><title>Studio Pocket design</title>\n");
        for(Part part:parts)for(double[][] path:part.paths()){
            body.append("<path fill=\"none\" stroke=\"#15443e\" stroke-width=\"0.25\" d=\"");
            boolean closed=path.length>2 && java.util.Arrays.equals(path[0],path[path.length-1]);
            for(int i=0;i<path.length-(closed?1:0);i++)
                body.append(String.format(Locale.ROOT,"%s%.5f %.5f ",i==0?"M":"L",path[i][0],height-path[i][1]));
            body.append(closed?"Z\"/>\n":"\"/>\n");
        }
        return body.append("</svg>\n").toString();
    }
}
