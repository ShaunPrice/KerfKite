package au.cncstudio.pocket;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.ViewGroup;

import android.widget.*;
import java.io.*;
import java.util.List;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import org.json.JSONException;

public final class WorkshopActivity extends Activity {
    private Design design;
    private LinearLayout page, items;
    private BedView preview;
    private ScreenAwakeSwitch screenAwake;
    private TextView feedback,projectTitle,dimensions;
    private Spinner head;
    private Button add;
    private DraftLibrary library;
    private DraftLibrary.Entry active;
    private final ExecutorService reader=Executors.newSingleThreadExecutor();
    private static final int OPEN_FILE=41;
    private Button edit, remove, project, svg;
    private SharedPreferences prefs;
    private static final int CREATE_FILE=40;
    private static final String PENDING="pending-export";
    private int dp(int v){return Math.round(getResources().getDisplayMetrics().density*v);}

    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        prefs=getSharedPreferences("workshop",MODE_PRIVATE);
        String warning="";
        library=new DraftLibrary(this);
        try{
            active=library.active();
            if(active==null){
                String legacy=prefs.getString("draft",null);
                design=legacy==null?new Design():Design.restore(legacy);
                active=library.create(legacy==null?"Untitled":"Pocket design",design);
                if(legacy!=null)prefs.edit().remove("draft").apply();
            } else design=library.load(active);
        }catch(Exception e){
            design=new Design();active=null;
            warning="The saved project could not be opened. Open another project or create a new one.";
        }
        ScrollView scroll=new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(0xfff5f7f5);
        scroll.setOnApplyWindowInsetsListener((v,insets)->{
            v.setPadding(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(),insets.getSystemWindowInsetBottom());
            return insets;
        });
        page=column();page.setPadding(dp(16),dp(12),dp(16),dp(24));scroll.addView(page);
        setContentView(scroll);
        TextView title=label("Studio Pocket",28);title.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
        page.addView(title);
        projectTitle=label("",20);page.addView(projectTitle);
        dimensions=label("",14);page.addView(dimensions);
        LinearLayout workspace=row();page.addView(workspace);
        workspace.addView(button("My projects",this::manage),weight());
        workspace.addView(button("Open file",this::browse),weight());
        page.addView(button("CNC pendant",()->startActivity(new Intent(this,PendantActivity.class))));
        screenAwake=new ScreenAwakeSwitch(this);page.addView(screenAwake);
        preview=new BedView(this,design,this::select);
        LinearLayout.LayoutParams previewSize=new LinearLayout.LayoutParams(-1,dp(205));
        previewSize.setMargins(0,dp(10),0,dp(10));page.addView(preview,previewSize);
        page.addView(label("Machine head",14));
        head=choices(new String[]{"CNC router","Laser","Drag knife"});
        head.setContentDescription("Machine head");head.setSelection(design.head);page.addView(head);
        onChoice(head,i->{if(active!=null && design.reference==null && i!=design.head){design.head=i;save();}});
        LinearLayout controls=row();page.addView(controls);
        add=button("Add shape",()->editor(false));controls.addView(add,weight());
        edit=button("Edit",()->editor(true));controls.addView(edit,weight());
        remove=button("Delete",this::deleteSelected);controls.addView(remove,weight());
        page.addView(label("Objects",18));items=column();page.addView(items);
        page.addView(label("Export design",18));LinearLayout formats=row();page.addView(formats);
        project=button("Studio project",()->export(false));svg=button("SVG",()->export(true));
        formats.addView(project,weight());formats.addView(svg,weight());
        feedback=label(warning,14); feedback.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);page.addView(feedback);
        page.addView(button("Quick guide",()->new AlertDialog.Builder(this).setTitle("Design on your phone")
                .setMessage("Use My projects to create, open, rename or duplicate local drawings. Open file imports a separate library entry.\n\n1. Tap Add shape. Choose a shape and enter its lower-left position and size in millimetres.\n\n2. Tap an object or its row to select it. Edit changes its dimensions.\n\n3. Choose a machine head, then Studio project. Open the saved file in CNC Studio to set cutting parameters and simulate it. SVG saves the artwork in millimetres.\n\n"+getString(R.string.keep_screen_awake_help))
                .setPositiveButton("Close",null).show()));
        refresh();
    }
    @Override protected void onResume(){super.onResume();screenAwake.resume();}
    @Override protected void onPause(){screenAwake.pause();super.onPause();}

    private LinearLayout column(){LinearLayout l=new LinearLayout(this);l.setOrientation(LinearLayout.VERTICAL);return l;}
    private LinearLayout row(){return new LinearLayout(this);}
    private LinearLayout.LayoutParams weight(){return new LinearLayout.LayoutParams(0,-2,1);}
    private TextView label(String s,int size){TextView t=new TextView(this);t.setText(s);t.setTextSize(size);t.setTextColor(0xff203a39);return t;}
    private Button button(String name,Runnable action){Button b=new Button(this);b.setText(name);b.setAllCaps(false);b.setOnClickListener(v->action.run());return b;}
    private Spinner choices(String[] options){
        Spinner s=new Spinner(this);ArrayAdapter<String> a=new ArrayAdapter<>(this,android.R.layout.simple_spinner_item,options);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);s.setAdapter(a);return s;
    }
    private void onChoice(Spinner spinner, java.util.function.IntConsumer chosen){
        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener(){
            public void onItemSelected(AdapterView<?> parent,View view,int i,long id){chosen.accept(i);}
            public void onNothingSelected(AdapterView<?> parent){}
        });
    }
    private boolean save(){
        if(active==null)return false;
        try{library.save(active,design);return true;}
        catch(Exception e){feedback.setText("Could not save draft: "+e.getMessage());return false;}
    }
    private void select(int index){design.selected=index;refresh();}
    private void refresh(){
        boolean editable=active!=null && design.reference==null;
        boolean selected=design.selected>=0 && design.selected<design.parts.size();
        projectTitle.setText(active==null?"No active project":active.name);
        dimensions.setText(String.format(Locale.ROOT,"%s × %s mm   ·   %s",trim(design.width),trim(design.height),
                design.reference!=null?"Desktop reference · read only":design.artwork?"Artwork copy · output off":"Plan view"));
        add.setEnabled(editable);head.setEnabled(editable);
        head.setSelection(design.head);preview.model=design;
        edit.setEnabled(editable && selected);remove.setEnabled(editable && selected);
        project.setEnabled(!design.parts.isEmpty());svg.setEnabled(editable && !design.parts.isEmpty());
        items.removeAllViews();
        if(design.parts.isEmpty())items.addView(label("Add a shape to start drawing.",14));
        for(int i=0;i<design.parts.size();i++){
            final int index=i;Part p=design.parts.get(i);
            Button b=button(String.format(Locale.ROOT,"%s · %.1f × %.1f mm",p.title(),p.width,p.height),()->select(index));
            b.setSelected(i==design.selected);b.setTextColor(i==design.selected?0xff006e65:0xff344646);
            if(i==design.selected)b.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
            items.addView(b);
        }
        preview.invalidate();
    }
    private EditText number(LinearLayout parent,String name,String value,boolean integer){
        TextView text=label(name,13);parent.addView(text);EditText e=new EditText(this);
        e.setId(View.generateViewId());text.setLabelFor(e.getId());
        e.setSingleLine();e.setInputType(InputType.TYPE_CLASS_NUMBER|(integer?0:InputType.TYPE_NUMBER_FLAG_DECIMAL));
        e.setText(value);e.setSelectAllOnFocus(true);parent.addView(e);return e;
    }
    private void editor(boolean editing){
        if(active==null || design.reference!=null)return;
        if(!editing && design.parts.size()>=128){feedback.setText("This drawing has reached 128 shapes.");return;}
        Part base=editing?design.parts.get(design.selected):new Part(0,6,design.width>=50?10:0,design.height>=35?10:0,Math.min(40,design.width),Math.min(25,design.height));
        LinearLayout form=column();form.setPadding(dp(20),0,dp(20),0);ScrollView scroller=new ScrollView(this);scroller.addView(form);
        Spinner type=choices(Part.TYPES);type.setContentDescription("Shape type");type.setSelection(base.type==Part.PATH?0:base.type);form.addView(type);if(base.type==Part.PATH)type.setVisibility(View.GONE);
        LinearLayout dims=row();form.addView(dims);LinearLayout a=column(),b=column();dims.addView(a,weight());dims.addView(b,weight());
        EditText width=number(a,"Width (mm)",Double.toString(base.width),false),height=number(b,"Height (mm)",Double.toString(base.height),false);
        EditText x=number(a,"X (mm)",Double.toString(base.x),false),y=number(b,"Y (mm)",Double.toString(base.y),false);
        LinearLayout sidesGroup=column();form.addView(sidesGroup);
        EditText sides=number(sidesGroup,"Polygon sides",Integer.toString(base.sides),true);
        onChoice(type,i->sidesGroup.setVisibility(i==2?View.VISIBLE:View.GONE));sidesGroup.setVisibility(base.type==2?View.VISIBLE:View.GONE);
        TextView error=label("",14);error.setTextColor(0xffa32d1d);form.addView(error);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle(editing?"Edit shape":"Add shape").setView(scroller)
            .setNegativeButton("Cancel",null).setPositiveButton("Apply",null).create();
        dialog.setOnShowListener(d->dialog.getButton(-1).setOnClickListener(v->{
            try{
                int shapeType=type.getSelectedItemPosition();
                Part part=base.type==Part.PATH?base.reposition(value(x),value(y),value(width),value(height)):
                    new Part(shapeType,shapeType==2?Integer.parseInt(sides.getText().toString()):base.sides,
                    value(x),value(y),value(width),value(height));
                part.checkBed(design.width,design.height);
                if(editing)design.parts.set(design.selected,part);else{design.parts.add(part);design.selected=design.parts.size()-1;}
                boolean saved=save();refresh();if(saved)feedback.setText("Draft saved.");dialog.dismiss();
            }catch(NumberFormatException e){error.setText("Enter a number in each field.");}
            catch(IllegalArgumentException e){error.setText(e.getMessage());}
        }));
        dialog.show();
    }
    private static double value(EditText field){return Double.parseDouble(field.getText().toString());}
    private void deleteSelected(){new AlertDialog.Builder(this).setMessage("Delete the selected shape?")
        .setNegativeButton("Cancel",null).setPositiveButton("Delete",(d,w)->{
            design.parts.remove(design.selected);design.selected=Math.min(design.selected,design.parts.size()-1);save();refresh();
        }).show();}
    private static String trim(double v){return v==(long)v?Long.toString((long)v):Double.toString(v);}
    private void manage(){
        LinearLayout menu=column();menu.setPadding(dp(16),0,dp(16),0);
        ScrollView scroll=new ScrollView(this);scroll.addView(menu);
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle("My projects").setView(scroll).setNegativeButton("Close",null).create();
        menu.addView(button("New project",()->{dialog.dismiss();nameDialog("New project","Untitled",name->create(name,new Design()));}));
        if(active!=null){
            menu.addView(button("Rename current",()->{dialog.dismiss();nameDialog("Rename project",active.name,name->{
                library.rename(active,name);active=library.active();refresh();
            });}));
            menu.addView(button("Duplicate current",()->{dialog.dismiss();nameDialog("Duplicate project",DraftLibrary.withSuffix(active.name," copy"),
                    name->create(name,Design.restore(design.draft().toString())));}));
            if(design.reference!=null)menu.addView(button("Extract editable artwork",()->{
                dialog.dismiss();
                new AlertDialog.Builder(this).setTitle("Create an artwork copy?")
                        .setMessage("Copy every displayed XY outline into a new editable drawing. The desktop reference keeps its original data. Review layers and cutting settings in Studio before enabling output for the copied objects.")
                        .setPositiveButton("Extract",(d,w)->{
                            try{create(DraftLibrary.withSuffix(active.name," artwork"),design.artworkCopy());}catch(Exception failure){showError(failure);}
                        }).setNegativeButton("Cancel",null).show();
            }));
            menu.addView(button("Delete current",()->{dialog.dismiss();confirmDelete();}));
        }
        menu.addView(label("Open a saved project",16));
        for(DraftLibrary.Entry entry:library.list()){
            Button item=button(entry.name,()->{
                try{Design loaded=library.load(entry);library.choose(entry);activate(entry,loaded);dialog.dismiss();}
                catch(Exception failure){showError(failure);}
            });
            if(active!=null && entry.id.equals(active.id))item.setTypeface(Typeface.DEFAULT,Typeface.BOLD);
            menu.addView(item);
        }
        dialog.show();
    }

    private interface NameAction{void apply(String name) throws Exception;}
    private void nameDialog(String title,String initial,NameAction action){
        EditText input=new EditText(this);input.setSingleLine();input.setText(initial);input.selectAll();
        input.setContentDescription("Project name");
        AlertDialog dialog=new AlertDialog.Builder(this).setTitle(title).setView(input)
                .setPositiveButton("Save",null).setNegativeButton("Cancel",null).create();
        dialog.setOnShowListener(d->dialog.getButton(-1).setOnClickListener(v->{
            try{action.apply(DraftLibrary.clean(input.getText().toString()));dialog.dismiss();}
            catch(Exception e){input.setError(e.getMessage());}
        }));dialog.show();
    }
    private void create(String name,Design model) throws Exception{activate(library.create(name,model),model);}
    private void activate(DraftLibrary.Entry entry,Design model){
        active=entry;design=model;refresh();feedback.setText("");
    }
    private void confirmDelete(){
        new AlertDialog.Builder(this).setTitle("Delete “"+active.name+"”?")
                .setMessage("This removes the copy stored in Pocket. Exported files stay in their original locations.")
                .setNegativeButton("Cancel",null).setPositiveButton("Delete",(d,w)->{
            try{
                DraftLibrary.Entry old=active, replacement=null;Design loaded=null;
                for(DraftLibrary.Entry next:library.list())if(!next.id.equals(old.id)){replacement=next;loaded=library.load(next);break;}
                if(replacement==null){loaded=new Design();replacement=library.create("Untitled",loaded);}
                library.delete(old,replacement);activate(replacement,loaded);
            }catch(Exception failure){showError(failure);}
        }).show();
    }
    private void showError(Exception e){new AlertDialog.Builder(this).setTitle("Project could not be opened")
            .setMessage(e.getMessage()).setPositiveButton("Close",null).show();}
    private void browse(){
        Intent intent=new Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*");
        startActivityForResult(intent,OPEN_FILE);
    }
    private void openDocument(android.net.Uri uri){
        feedback.setText("Reading project…");
        reader.execute(()->{
            try(InputStream in=getContentResolver().openInputStream(uri)){
                if(in==null)throw new IOException("Document is unavailable.");
                ByteArrayOutputStream out=new ByteArrayOutputStream();byte[] buffer=new byte[8192];int count;
                while((count=in.read(buffer))!=-1){
                    if(out.size()+count>Design.MAX_BYTES)throw new IOException("Project exceeds the 4 MB portable limit.");
                    out.write(buffer,0,count);
                }
                String source=StandardCharsets.UTF_8.newDecoder().decode(java.nio.ByteBuffer.wrap(out.toByteArray())).toString();
                JSONObject doc=ProjectFile.parse(source);
                JSONObject metadata=doc.optJSONObject("metadata");String name=metadata==null?"Imported project":metadata.optString("name","Imported project").trim();
                if(name.isEmpty())name="Imported project";
                final String projectName=DraftLibrary.clean(name);
                Design imported=Design.importProject(source,doc,projectName);
                runOnUiThread(()->{
                    if(isFinishing()||isDestroyed())return;
                    feedback.setText("");
                    String message=imported.parts.size()+" objects · "+trim(imported.width)+" × "+trim(imported.height)+" mm";
                    if(imported.reference!=null)message+="\n\nOpen as a read-only desktop reference. Pocket can display its XY outlines and export the complete original project. Edit its layers, CAD features and machining settings in Studio.";
                    new AlertDialog.Builder(this).setTitle("Open “"+projectName+"”?").setMessage(message)
                       .setNegativeButton("Cancel",null).setPositiveButton("Open copy",(d,w)->{
                        try{create(projectName,imported);}catch(Exception failure){showError(failure);}
                    }).show();
                });
            }catch(Exception failure){runOnUiThread(()->{if(!isFinishing()&&!isDestroyed()){feedback.setText("");showError(failure);}});}
        });
    }
    @Override protected void onDestroy(){reader.shutdownNow();super.onDestroy();}
    private void export(boolean asSvg){
        try{
            String payload=asSvg?design.svg():design.project(active.name);
            if(!prefs.edit().putString(PENDING,payload).commit())throw new java.io.IOException("Cannot stage file");
            Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
               .setType(asSvg?"image/svg+xml":"application/json").putExtra(Intent.EXTRA_TITLE,active.name.replaceAll("[^\\p{L}\\p{N}._ -]","_")+(asSvg?".svg":".cncstudio.json"));
            startActivityForResult(intent,CREATE_FILE);
        }catch(Exception e){feedback.setText("Export failed: "+e.getMessage());}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
        if(request==OPEN_FILE){
            if(result==RESULT_OK && data!=null && data.getData()!=null)openDocument(data.getData());
            return;
        }
        if(request!=CREATE_FILE)return;
        String payload=prefs.getString(PENDING,null);
        prefs.edit().remove(PENDING).apply();
        if(result!=RESULT_OK || data==null || data.getData()==null)return;
        try(OutputStream output=getContentResolver().openOutputStream(data.getData(),"wt")){
            if(output==null || payload==null)throw new java.io.IOException("Document unavailable");
            output.write(payload.getBytes(StandardCharsets.UTF_8));output.flush();
        }catch(Exception e){feedback.setText("Export failed: "+e.getMessage());return;}
        feedback.setText("Design exported.");
    }
}
