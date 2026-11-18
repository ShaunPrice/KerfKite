package au.cncstudio.pocket;

import org.json.JSONException;
import org.json.JSONObject;
import org.json.JSONTokener;

final class ProjectFile {
    static JSONObject parse(String text) throws JSONException {
        int depth=0;boolean quoted=false,escaped=false;
        for(int i=0;i<text.length();i++){
            char c=text.charAt(i);
            if(quoted){
                if(escaped)escaped=false;
                else if(c=='\\')escaped=true;
                else if(c=='"')quoted=false;
            }else if(c=='"')quoted=true;
            else if(c=='{'||c=='['){
                if(++depth>96)throw new JSONException("Project is nested too deeply.");
            }else if(c=='}'||c==']'){
                if(--depth<0)throw new JSONException("Invalid project JSON.");
            }
        }
        if(depth!=0||quoted)throw new JSONException("Incomplete project file.");
        JSONTokener input=new JSONTokener(text);
        Object value=input.nextValue();
        if(!(value instanceof JSONObject)||input.nextClean()!=0)throw new JSONException("Expected one Studio project.");
        return (JSONObject)value;
    }
}
