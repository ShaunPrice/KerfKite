package au.cncstudio.pocket;

import android.content.Context;
import android.content.SharedPreferences;
import android.util.AtomicFile;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.List;
import java.util.UUID;
import org.json.JSONException;

final class DraftLibrary {
    static final class Entry {
        final String id,name;
        Entry(String id,String name){this.id=id;this.name=name;}
        @Override public String toString(){return name;}
    }
    private final File folder;
    private final SharedPreferences index;
    private static final String ACTIVE="active",PREFIX="name.";
    DraftLibrary(Context context){
        folder=new File(context.getFilesDir(),"projects");
        index=context.getSharedPreferences("project-library",Context.MODE_PRIVATE);
    }
    List<Entry> list(){
        List<Entry> entries=new ArrayList<>();
        for(String key:index.getAll().keySet())if(key.startsWith(PREFIX))
            entries.add(new Entry(key.substring(PREFIX.length()),index.getString(key,"")));
        Collections.sort(entries,Comparator.comparing((Entry e)->e.name,String.CASE_INSENSITIVE_ORDER).thenComparing(e->e.id));
        return entries;
    }
    Entry active(){
        String id=index.getString(ACTIVE,null);
        return id==null?null:new Entry(id,index.getString(PREFIX+id,"Untitled"));
    }
    void choose(Entry entry) throws IOException {commit(index.edit().putString(ACTIVE,entry.id));}
    void rename(Entry entry,String name) throws IOException {
        name=clean(name);
        for(Entry existing:list())if(!existing.id.equals(entry.id)&&existing.name.equalsIgnoreCase(name))
            throw new IllegalArgumentException("That project name is already in use.");
        commit(index.edit().putString(PREFIX+entry.id,name));
    }
    static String withSuffix(String name,String suffix){
        return name.substring(0,Math.min(name.length(),100-suffix.length()))+suffix;
    }
    Entry create(String name, Design model) throws IOException,JSONException{
        String id=UUID.randomUUID().toString();
        Entry entry=new Entry(id,available(clean(name)));
        save(entry,model);
        try { commit(index.edit().putString(PREFIX+id,entry.name).putString(ACTIVE,id)); }
        catch(IOException failure){file(id).delete();throw failure;}
        return entry;
    }
    private String available(String name){
        java.util.Set<String> existing=new java.util.HashSet<>();
        for(Entry e:list())existing.add(e.name.toLowerCase(java.util.Locale.ROOT));
        String candidate=name;
        for(int n=2;existing.contains(candidate.toLowerCase(java.util.Locale.ROOT));n++)candidate=withSuffix(name," ("+n+")");
        return candidate;
    }
    static String clean(String s){
        String name=s.trim();
        if(name.isEmpty()||name.length()>100||name.matches("(?s).*\\p{Cntrl}.*"))
            throw new IllegalArgumentException("Use a project name of 1–100 characters.");
        return name;
    }
    void delete(Entry entry, Entry replacement) throws IOException{
        if(entry.id.equals(replacement.id))throw new IllegalArgumentException("Choose another project first.");
        commit(index.edit().remove(PREFIX+entry.id).putString(ACTIVE,replacement.id));
        file(entry.id).delete();
    }
    private static void commit(SharedPreferences.Editor editor) throws IOException{
        if(!editor.commit())throw new IOException("The phone could not save the project index.");
    }
    private AtomicFile file(String id){
        UUID.fromString(id);
        return new AtomicFile(new File(folder,id+".json"));
    }
    Design load(Entry entry) throws IOException,JSONException{
        return Design.restore(new String(file(entry.id).readFully(),StandardCharsets.UTF_8));
    }
    void save(Entry entry,Design model) throws IOException,JSONException{
        if(!folder.isDirectory()&&!folder.mkdirs())throw new IOException("The project folder is unavailable.");
        byte[] data=model.draft().toString().getBytes(StandardCharsets.UTF_8);
        AtomicFile file=file(entry.id);FileOutputStream out=file.startWrite();
        try{out.write(data);file.finishWrite(out);}
        catch(IOException failure){file.failWrite(out);throw failure;}
    }
}
