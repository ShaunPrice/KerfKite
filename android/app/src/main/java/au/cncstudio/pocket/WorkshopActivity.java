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
import android.view.WindowInsets;
import android.widget.*;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import org.json.JSONException;

public final class WorkshopActivity extends Activity {
    private Design design;
    private LinearLayout page, items;
    private BedView preview;
    private TextView feedback;
    private Button edit, remove, project, svg;
    private SharedPreferences prefs;
    private static final int CREATE_FILE=40;
    private static final String PENDING="pending-export";
    private int dp(int v){return Math.round(getResources().getDisplayMetrics().density*v);}

    @Override public void onCreate(Bundle state){
        super.onCreate(state);
        prefs=getSharedPreferences("workshop",MODE_PRIVATE);
        String warning="";
        try {design=Design.restore(prefs.getString("draft","{\"parts\":[]}"));}
        catch(Exception e){design=new Design();warning="Saved draft could not be opened.";}
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
        page.addView(title);page.addView(label("680 × 240 mm   ·   Plan view",14));
        preview=new BedView(this,design,this::select);
        LinearLayout.LayoutParams previewSize=new LinearLayout.LayoutParams(-1,dp(205));
        previewSize.setMargins(0,dp(10),0,dp(10));page.addView(preview,previewSize);
        page.addView(label("Machine head",14));
        Spinner head=choices(new String[]{"CNC router","Laser","Drag knife"});
        head.setContentDescription("Machine head");head.setSelection(design.head);page.addView(head);
        onChoice(head,i->{design.head=i;save();});
        LinearLayout controls=row();page.addView(controls);
        controls.addView(button("Add shape",()->editor(false)),weight());
        edit=button("Edit",()->editor(true));controls.addView(edit,weight());
        remove=button("Delete",this::deleteSelected);controls.addView(remove,weight());
        page.addView(label("Objects",18));items=column();page.addView(items);
        page.addView(label("Export design",18));LinearLayout formats=row();page.addView(formats);
        project=button("Studio project",()->export(false));svg=button("SVG",()->export(true));
        formats.addView(project,weight());formats.addView(svg,weight());
        feedback=label(warning,14); feedback.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);page.addView(feedback);
        page.addView(button("Quick guide",()->new AlertDialog.Builder(this).setTitle("Design on your phone")
                .setMessage("1. Tap Add shape. Choose a shape and enter its lower-left position and size in millimetres.\n\n2. Tap an object or its row to select it. Edit changes its dimensions.\n\n3. Choose a machine head, then Studio project. Open the saved file in CNC Studio to set cutting parameters and simulate it. SVG saves the artwork in millimetres.")
                .setPositiveButton("Close",null).show()));
        refresh();
    }
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
    private void save(){
        try{prefs.edit().putString("draft",design.draft().toString()).apply();}
        catch(JSONException e){if(feedback!=null)feedback.setText("Could not save draft.");}
    }
    private void select(int index){design.selected=index;refresh();save();}
    private void refresh(){
        boolean selected=design.selected>=0 && design.selected<design.parts.size();
        edit.setEnabled(selected); remove.setEnabled(selected);
        project.setEnabled(!design.parts.isEmpty());svg.setEnabled(!design.parts.isEmpty());
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
        if(!editing && design.parts.size()>=128){feedback.setText("This drawing has reached 128 shapes.");return;}
        Part base=editing?design.parts.get(design.selected):new Part(0,6,10,10,40,25);
        LinearLayout form=column();form.setPadding(dp(20),0,dp(20),0);ScrollView scroller=new ScrollView(this);scroller.addView(form);
        Spinner type=choices(Part.TYPES);type.setContentDescription("Shape type");type.setSelection(base.type);form.addView(type);
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
                Part part=new Part(shapeType,shapeType==2?Integer.parseInt(sides.getText().toString()):base.sides,
                    value(x),value(y),value(width),value(height));
                if(editing)design.parts.set(design.selected,part);else{design.parts.add(part);design.selected=design.parts.size()-1;}
                save();refresh();feedback.setText("Draft saved.");dialog.dismiss();
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
    private void export(boolean asSvg){
        try{
            String payload=asSvg?design.svg():design.project();
            if(!prefs.edit().putString(PENDING,payload).commit())throw new java.io.IOException("Cannot stage file");
            Intent intent=new Intent(Intent.ACTION_CREATE_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
               .setType(asSvg?"image/svg+xml":"application/json").putExtra(Intent.EXTRA_TITLE,asSvg?"pocket-design.svg":"pocket-design.cncstudio.json");
            startActivityForResult(intent,CREATE_FILE);
        }catch(Exception e){feedback.setText("Export failed: "+e.getMessage());}
    }
    @Override protected void onActivityResult(int request,int result,Intent data){
        super.onActivityResult(request,result,data);
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
