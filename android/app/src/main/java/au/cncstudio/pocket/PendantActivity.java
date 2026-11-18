package au.cncstudio.pocket;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.text.InputType;
import android.view.View;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import org.json.JSONObject;

public final class PendantActivity extends Activity {
    private static final double[] STEPS={0.1,0.5,1,5,10};
    private static final int[] FEEDS={50,100,250,500,1000};
    private static final long FRESH_MS=3500;
    private final Handler main=new Handler(Looper.getMainLooper());
    private final ExecutorService network=Executors.newSingleThreadExecutor();
    // A Stop travels independently of an ordinary motion POST still waiting for a reply.
    private final ExecutorService priority=Executors.newSingleThreadExecutor();
    private final Map<Button,String> commands=new LinkedHashMap<>();
    private volatile int generation;
    private volatile boolean foreground;
    private volatile long updated;
    private PendantClient client;
    private JSONObject snapshot;
    private boolean polling, staleFeedback, startUnresolved;
    private String pendingAction;
    private final Runnable pollTick=this::poll;
    private LinearLayout page,pairFields;
    private EditText link;
    private TextView machine,position,armed,feedback;
    private Button disconnect;
    private ScreenAwakeSwitch screenAwake;
    private Spinner increment,feed;
    private AlertDialog confirm;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        ScrollView scroll=new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(0xfff5f7f5);
        scroll.setOnApplyWindowInsetsListener((v,insets)->{
            v.setPadding(insets.getSystemWindowInsetLeft(),insets.getSystemWindowInsetTop(),
                    insets.getSystemWindowInsetRight(),insets.getSystemWindowInsetBottom());return insets;
        });
        page=column();page.setPadding(dp(16),dp(12),dp(16),dp(24));scroll.addView(page);setContentView(scroll);
        title("CNC pendant",26);
        page.addView(text("Reach the Studio receiver over Wi-Fi, USB tethering or Bluetooth tethering. Paste the pairing link shown by Studio.",15));
        pairFields=column();page.addView(pairFields);
        pairFields.addView(text("Pair with the machine receiver",17));
        link=new EditText(this);
        link.setHint("Monitor pairing link");link.setContentDescription("Monitor pairing link");
        link.setSingleLine();link.setSaveEnabled(false);
        link.setInputType(InputType.TYPE_CLASS_TEXT|InputType.TYPE_TEXT_VARIATION_PASSWORD|
                InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        link.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO_EXCLUDE_DESCENDANTS);
        pairFields.addView(link);pairFields.addView(button("Pair",this::pair));
        machine=text("Unpaired",20);machine.setContentDescription("Machine status");page.addView(machine);
        position=text("Position unavailable",16);position.setContentDescription("Controller position unavailable");page.addView(position);
        armed=text("",15);page.addView(armed);
        feedback=text("",15);feedback.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        page.addView(feedback);
        screenAwake=new ScreenAwakeSwitch(this);page.addView(screenAwake);
        title("Jog",19);
        LinearLayout options=row(),steps=column(),rates=column();
        page.addView(options);options.addView(steps,half());options.addView(rates,half());
        steps.addView(text("Increment · mm",14)); rates.addView(text("Feed · mm/min",14));
        increment=spinner("Jog increment (mm)",new String[]{"0.1","0.5","1","5","10"});
        feed=spinner("Jog feed (mm/min)",new String[]{"50","100","250","500","1000"});
        feed.setSelection(1);steps.addView(increment);rates.addView(feed);
        for(String axis:new String[]{"X","Y","Z"}) {
            LinearLayout group=row();page.addView(group);
            for(int sign:new int[]{-1,1}) {
                Button b=commandButton("jog",axis+(sign<0?" −":" +"),()->jog(axis,sign));
                b.setContentDescription("Jog "+axis+(sign<0?" negative":" positive"));group.addView(b,half());
            }
        }
        LinearLayout origin=row();page.addView(origin);
        origin.addView(commandButton("home","Home",()->ask("home","Home","Home the connected machine? The axes will move.")),half());
        origin.addView(commandButton("zero","Zero XYZ",()->ask("zero","Zero XYZ","Set the current work coordinates to zero on all three axes?")),half());
        title("Job control",19);
        LinearLayout run=row(),hold=row();page.addView(run);page.addView(hold);
        run.addView(commandButton("start","Start",()->ask("start","Start","Start the preview armed at the receiver? Check the machine area is clear.")),half());
        run.addView(commandButton("stop","Stop",()->ask("stop","Stop","Abort the job and disarm the preview? Stopped jobs cannot resume.")),half());
        hold.addView(commandButton("pause","Pause",()->send("pause",null,0,0)),half());
        hold.addView(commandButton("resume","Resume",()->send("resume",null,0,0)),half());
        disconnect=button("Forget pairing",()->forget("Pairing cleared."));page.addView(disconnect);
        updateControls();
    }

    private int dp(int value) {return Math.round(getResources().getDisplayMetrics().density*value);}
    private LinearLayout column(){LinearLayout layout=new LinearLayout(this);layout.setOrientation(LinearLayout.VERTICAL);return layout;}
    private LinearLayout row(){return new LinearLayout(this);}
    private LinearLayout.LayoutParams half(){return new LinearLayout.LayoutParams(0,-2,1);}
    private TextView text(String label,int sp){TextView t=new TextView(this);t.setText(label);t.setTextSize(sp);t.setTextColor(0xff203a39);return t;}
    private void title(String label,int size){TextView t=text(label,size);t.setTypeface(Typeface.DEFAULT,Typeface.BOLD);t.setPadding(0,dp(8),0,dp(4));page.addView(t);}
    private Button button(String label,Runnable onTap){Button b=new Button(this);b.setText(label);b.setAllCaps(false);b.setMinHeight(dp(48));b.setOnClickListener(v->onTap.run());return b;}
    private Button commandButton(String action,String label,Runnable onTap){Button b=button(label,onTap);commands.put(b,action);return b;}
    private Spinner spinner(String name,String[] values){
        Spinner s=new Spinner(this);s.setContentDescription(name);
        ArrayAdapter<String> adapter=new ArrayAdapter<>(this,android.R.layout.simple_spinner_item,values);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);s.setAdapter(adapter);return s;
    }

    private void pair(){
        PendantClient candidate;
        try {candidate=new PendantClient(link.getText().toString());}
        catch(IllegalArgumentException e){feedback.setText(e.getMessage());return;}
        if(candidate.cleartext){
            confirm=new AlertDialog.Builder(this).setTitle("Use unencrypted HTTP?")
                    .setMessage("The pairing token and commands can be read on this network. Use HTTP only on a trusted private link to "+candidate.origin+".")
                    .setNegativeButton("Cancel",null).setPositiveButton("Use HTTP",(d,w)->connect(candidate)).show();
        } else connect(candidate);
    }
    private void connect(PendantClient candidate){
        forget("");client=candidate;link.setText("");pairFields.setVisibility(View.GONE);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_SECURE);
        machine.setContentDescription("Machine status: Connecting");machine.setText("Connecting");updateControls();poll();
    }
    private void forget(String message){
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_SECURE);
        ++generation;client=null;updated=0;snapshot=null;pendingAction=null;startUnresolved=false;polling=false;
        main.removeCallbacksAndMessages(null);link.setText("");pairFields.setVisibility(View.VISIBLE);
        machine.setContentDescription("Machine status: Unpaired");machine.setText("Unpaired");
        position.setContentDescription("Controller position unavailable");position.setText("Position unavailable");armed.setText("");feedback.setText(message);updateControls();
    }

    private boolean fresh(){return foreground && updated>0 && SystemClock.elapsedRealtime()-updated<FRESH_MS;}
    private String controlEpoch(){
        Object epoch=snapshot==null?null:snapshot.opt("control_epoch");
        return epoch instanceof String?(String)epoch:"";
    }
    private boolean stopMayPreempt(){
        return "start".equals(pendingAction) || "jog".equals(pendingAction);
    }
    private boolean permitted(String action){
        if(!foreground || client==null)return false;
        if("stop".equals(action) && (stopMayPreempt() || (pendingAction==null && startUnresolved)))return true;
        if(!fresh() || snapshot==null || pendingAction!=null || !snapshot.optBoolean("connected"))return false;
        if(!"stop".equals(action) && controlEpoch().isEmpty())return false;
        String state=snapshot.optString("state");
        boolean active=snapshot.optBoolean("active"), idle="Idle".equalsIgnoreCase(state);
        boolean held=state.equalsIgnoreCase("Hold")||state.startsWith("Hold:");
        if(action.equals("jog")||action.equals("home")||action.equals("zero"))
            return !active && idle && snapshot.optBoolean("pendant");
        if(!snapshot.optBoolean("controls"))return false;
        switch(action){
            case "start":return !startUnresolved && idle && !active && snapshot.optBoolean("armed");
            case "pause":return active && !held;
            case "resume":return active && held;
            case "stop":return active || snapshot.optBoolean("armed") || held || "Jog".equals(state) || "Home".equals(state) || "Run".equals(state);
            default:return false;
        }
    }
    private void updateControls(){
        for(Map.Entry<Button,String> entry:commands.entrySet())entry.getKey().setEnabled(permitted(entry.getValue()));
        if(disconnect!=null)disconnect.setVisibility(client==null?View.GONE:View.VISIBLE);
    }
    private void stale(String reason){
        snapshot=null;updated=0;staleFeedback=true;machine.setContentDescription("Live status unavailable");machine.setText("Live status unavailable");
        position.setContentDescription("Controller position unavailable");position.setText("Position unavailable");armed.setText("");
        feedback.setText(reason);updateControls();
    }
    private void showState(JSONObject state){
        if(staleFeedback){feedback.setText("");staleFeedback=false;}
        String protocol=state.optString("protocol","");
        String mode=state.optBoolean("simulated")?"Simulator":"Machine";
        String current=state.optBoolean("connected")?state.optString("state","Unknown"):"Disconnected";
        machine.setText(mode+" · "+current+(protocol.isEmpty()?"":" · "+protocol));
        machine.setContentDescription("Machine status: "+machine.getText());
        JSONObject xyz=state.optJSONObject("position");
        if(xyz!=null && Double.isFinite(xyz.optDouble("x")) && Double.isFinite(xyz.optDouble("y")) && Double.isFinite(xyz.optDouble("z"))){
            String basis=xyz.optString("basis","");
            if("WPos".equals(basis))basis="Work";else if("MPos".equals(basis))basis="Machine";
            position.setText(String.format(Locale.ROOT,"%sX %.3f   Y %.3f   Z %.3f mm",
                    basis.isEmpty()?"":basis+" · ",xyz.optDouble("x"),xyz.optDouble("y"),xyz.optDouble("z")));
        }else position.setText("Position unavailable");
        position.setContentDescription("Controller position: "+position.getText());
        String control=state.optBoolean("pendant")?"Manual controls enabled":"Manual controls unavailable";
        String job=state.optBoolean("armed")?"Job armed":state.optBoolean("active")?
                "Job "+state.optInt("progress")+"%":"No job armed";
        if(!state.optBoolean("armed") || state.optBoolean("active"))startUnresolved=false;
        if(controlEpoch().isEmpty() && state.optBoolean("controls"))
            control="Update the Studio receiver for motion controls";
        armed.setText(control+" · "+job);
        if(!state.isNull("error") && !state.optString("error").isEmpty())feedback.setText(state.optString("error"));
    }
    private void poll(){poll(network);}
    private void poll(ExecutorService worker){
        if(!foreground || client==null || polling || pendingAction!=null)return;
        polling=true;int ticket=generation;
        PendantClient session=client;
        worker.execute(()->{
            try {
                JSONObject state=session.status();
                main.post(()->{
                    if(ticket!=generation || !foreground)return;
                    polling=false;snapshot=state;updated=SystemClock.elapsedRealtime();
                    showState(state);updateControls();schedule(ticket);
                });
            }catch(Exception e){
                main.post(()->{
                    if(ticket!=generation || !foreground)return;
                    polling=false;
                    if(e instanceof PendantClient.PairingExpired)forget(readable(e));
                    else{stale("Status unavailable. "+readable(e));schedule(ticket);}
                });
            }
        });
    }
    private void schedule(int ticket){
        main.removeCallbacks(pollTick);main.postDelayed(pollTick,1000);
        long sample=updated;
        if(sample>0)main.postDelayed(()->{
            if(ticket==generation && sample==updated && !fresh())stale("Waiting for a fresh receiver status.");
        },FRESH_MS);
    }

    private void jog(String axis,int sign){
        double distance=STEPS[increment.getSelectedItemPosition()]*sign;
        int rate=FEEDS[feed.getSelectedItemPosition()];
        send("jog",axis,distance,rate);
    }
    private void ask(String action,String label,String prompt){
        if(!permitted(action))return;
        final int decision=generation;
        final String chosenEpoch=controlEpoch();
        final boolean pendingStop="stop".equals(action) && (stopMayPreempt() || startUnresolved);
        confirm=new AlertDialog.Builder(this).setTitle(label).setMessage(prompt)
                .setNegativeButton("Cancel",null).setPositiveButton(label,(dialog,which)->{
                    if(decision!=generation || (!"stop".equals(action) && !chosenEpoch.equals(controlEpoch()))){
                        feedback.setText("Receiver state changed. Review it before confirming again.");return;
                    }
                    send(action,null,0,0,pendingStop);
                }).show();
    }
    private static String readable(Exception e){
        if(e instanceof org.json.JSONException)return "The monitor returned an unreadable response.";
        if(e instanceof javax.net.ssl.SSLException)return "Check the monitor certificate, trusted CA and server name.";
        if(e instanceof java.net.SocketTimeoutException)return "The receiver timed out.";
        if(e instanceof java.net.UnknownHostException || e instanceof java.net.ConnectException)return "Check the monitor address and network connection.";
        String message=e.getMessage();return message==null?"Receiver request failed.":message;
    }
    private void send(String action,String axis,double distance,int rate){
        send(action,axis,distance,rate,false);
    }
    private void send(String action,String axis,double distance,int rate,boolean confirmedPendingStop){
        boolean stop="stop".equals(action);
        if(!foreground || client==null || (!(confirmedPendingStop && stop) && !permitted(action))){
            feedback.setText("Wait for a fresh, permitted receiver state.");return;
        }
        final String epoch=controlEpoch();
        final long statusTime=updated;
        final int ticket=++generation;
        final PendantClient session=client;
        final ExecutorService worker=stop?priority:network;
        pendingAction=action;polling=false;main.removeCallbacks(pollTick);
        if("start".equals(action)){
            startUnresolved=true;
            feedback.setText("Start requested. Stop is available while the receiver confirms the job.");
        }
        updateControls();
        worker.execute(()->{
            if(ticket!=generation || !foreground)return;
            String message;
            if(!stop && SystemClock.elapsedRealtime()-statusTime>=FRESH_MS)
                message="Status expired before sending. Check the current machine state.";
            else try{
                session.command(action,axis,distance,rate,epoch);
                message="Receiver acknowledged "+action+".";
            }catch(Exception e){
                message=readable(e)+" Check the machine state before trying another action.";
            }
            final String result=message;
            main.post(()->{
                // A superseding Stop, new pairing, or background transition owns subsequent UI.
                if(ticket!=generation || !foreground)return;
                pendingAction=null;updated=0;snapshot=null;feedback.setText(result);updateControls();
                poll(worker); // A delayed ordinary POST cannot obstruct the status check after Stop.
            });
        });
    }
    @Override protected void onResume(){super.onResume();screenAwake.resume();foreground=true;updateControls();poll();}
    @Override protected void onPause(){
        screenAwake.pause();
        foreground=false;++generation;updated=0;snapshot=null;polling=false;pendingAction=null;main.removeCallbacksAndMessages(null);
        if(confirm!=null && confirm.isShowing())confirm.dismiss();
        updateControls();super.onPause();
    }
    @Override protected void onDestroy(){client=null;network.shutdownNow();priority.shutdownNow();super.onDestroy();}
}
